/*
 * Rufus: The Reliable USB Formatting Utility
 * Standard Windows function calls
 * Copyright © 2013-2024 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <sddl.h>
#include <gpedit.h>
#include <assert.h>
#include <accctrl.h>
#include <aclapi.h>

#include "re.h"
#include "rufus.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"

#include "settings.h"

// MinGW doesn't yet know these (from wldp.h)
typedef enum WLDP_WINDOWS_LOCKDOWN_MODE
{
	WLDP_WINDOWS_LOCKDOWN_MODE_UNLOCKED = 0,
	WLDP_WINDOWS_LOCKDOWN_MODE_TRIAL,
	WLDP_WINDOWS_LOCKDOWN_MODE_LOCKED,
	WLDP_WINDOWS_LOCKDOWN_MODE_MAX,
} WLDP_WINDOWS_LOCKDOWN_MODE, * PWLDP_WINDOWS_LOCKDOWN_MODE;

windows_version_t WindowsVersion = { 0 };

/*
 * Hash table functions - modified From glibc 2.3.2:
 * [Aho,Sethi,Ullman] Compilers: Principles, Techniques and Tools, 1986
 * [Knuth]            The Art of Computer Programming, part 3 (6.4)
 */

/*
 * For the used double hash method the table size has to be a prime. To
 * correct the user given table size we need a prime test.  This trivial
 * algorithm is adequate because the code is called only during init and
 * the number is likely to be small
 */
static uint32_t isprime(uint32_t number)
{
	// no even number will be passed
	uint32_t divider = 3;

	while((divider * divider < number) && (number % divider != 0))
		divider += 2;

	return (number % divider != 0);
}

/*
 * Before using the hash table we must allocate memory for it.
 * We allocate one element more as the found prime number says.
 * This is done for more effective indexing as explained in the
 * comment for the hash function.
 */
BOOL htab_create(uint32_t nel, htab_table* htab)
{
	if (htab == NULL) {
		return FALSE;
	}
	if_not_assert(htab->table == NULL) {
		uprintf("Warning: htab_create() was called with a non empty table");
		return FALSE;
	}

	// Change nel to the first prime number not smaller as nel.
	nel |= 1;
	while(!isprime(nel))
		nel += 2;

	htab->size = nel;
	htab->filled = 0;

	// allocate memory and zero out.
	htab->table = (htab_entry*)calloc(htab->size + 1, sizeof(htab_entry));
	if (htab->table == NULL) {
		uprintf("Could not allocate space for hash table");
		return FALSE;
	}

	return TRUE;
}

/* After using the hash table it has to be destroyed.  */
void htab_destroy(htab_table* htab)
{
	size_t i;

	if ((htab == NULL) || (htab->table == NULL)) {
		return;
	}

	for (i=0; i<htab->size+1; i++) {
		if (htab->table[i].used) {
			safe_free(htab->table[i].str);
		}
	}
	htab->filled = 0; htab->size = 0;
	safe_free(htab->table);
	htab->table = NULL;
}

/*
 * This is the search function. It uses double hashing with open addressing.
 * We use a trick to speed up the lookup. The table is created with one
 * more element available. This enables us to use the index zero special.
 * This index will never be used because we store the first hash index in
 * the field used where zero means not used. Every other value means used.
 * The used field can be used as a first fast comparison for equality of
 * the stored and the parameter value. This helps to prevent unnecessary
 * expensive calls of strcmp.
 */
uint32_t htab_hash(char* str, htab_table* htab)
{
	uint32_t hval, hval2;
	uint32_t idx;
	uint32_t r = 0;
	int c;
	char* sz = str;

	if ((htab == NULL) || (htab->table == NULL) || (str == NULL)) {
		return 0;
	}

	// Compute main hash value using sdbm's algorithm (empirically
	// shown to produce half the collisions as djb2's).
	// See http://www.cse.yorku.ca/~oz/hash.html
	while ((c = *sz++) != 0)
		r = c + (r << 6) + (r << 16) - r;
	if (r == 0)
		++r;

	// compute table hash: simply take the modulus
	hval = r % htab->size;
	if (hval == 0)
		++hval;

	// Try the first index
	idx = hval;

	if (htab->table[idx].used) {
		if ( (htab->table[idx].used == hval)
		  && (safe_strcmp(str, htab->table[idx].str) == 0) ) {
			// existing hash
			return idx;
		}
		// uprintf("Hash collision ('%s' vs '%s')", str, htab->table[idx].str);

		// Second hash function, as suggested in [Knuth]
		hval2 = 1 + hval % (htab->size - 2);

		do {
			// Because size is prime this guarantees to step through all available indexes
			if (idx <= hval2) {
				idx = ((uint32_t)htab->size) + idx - hval2;
			} else {
				idx -= hval2;
			}

			// If we visited all entries leave the loop unsuccessfully
			if (idx == hval) {
				break;
			}

			// If entry is found use it.
			if ( (htab->table[idx].used == hval)
			  && (safe_strcmp(str, htab->table[idx].str) == 0) ) {
				return idx;
			}
		}
		while (htab->table[idx].used);
	}

	// Not found => New entry

	// If the table is full return an error
	if_not_assert(htab->filled < htab->size) {
		uprintf("Hash table is full (%d entries)", htab->size);
		return 0;
	}

	safe_free(htab->table[idx].str);
	htab->table[idx].used = hval;
	htab->table[idx].str = (char*) malloc(safe_strlen(str) + 1);
	if (htab->table[idx].str == NULL) {
		uprintf("Could not duplicate string for hash table");
		return 0;
	}
	memcpy(htab->table[idx].str, str, safe_strlen(str) + 1);
	++htab->filled;

	return idx;
}

static const char* GetEdition(DWORD ProductType)
{
	static char unknown_edition_str[64];

	// From: https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getproductinfo
	// These values can be found in the winnt.h header.
	switch (ProductType) {
	case 0x00000000: return "";	//  Undefined
	case 0x00000001: return "Ultimate";
	case 0x00000002: return "Home Basic";
	case 0x00000003: return "Home Premium";
	case 0x00000004: return "Enterprise";
	case 0x00000005: return "Home Basic N";
	case 0x00000006: return "Business";
	case 0x00000007: return "Server Standard";
	case 0x00000008: return "Server Datacenter";
	case 0x00000009: return "Smallbusiness Server";
	case 0x0000000A: return "Server Enterprise";
	case 0x0000000B: return "Starter";
	case 0x0000000C: return "Server Datacenter (Core)";
	case 0x0000000D: return "Server Standard (Core)";
	case 0x0000000E: return "Server Enterprise (Core)";
	case 0x00000010: return "Business N";
	case 0x00000011: return "Web Server";
	case 0x00000012: return "HPC Edition";
	case 0x00000013: return "Storage Server (Essentials)";
	case 0x0000001A: return "Home Premium N";
	case 0x0000001B: return "Enterprise N";
	case 0x0000001C: return "Ultimate N";
	case 0x00000022: return "Home Server";
	case 0x00000024: return "Server Standard without Hyper-V";
	case 0x00000025: return "Server Datacenter without Hyper-V";
	case 0x00000026: return "Server Enterprise without Hyper-V";
	case 0x00000027: return "Server Datacenter without Hyper-V (Core)";
	case 0x00000028: return "Server Standard without Hyper-V (Core)";
	case 0x00000029: return "Server Enterprise without Hyper-V (Core)";
	case 0x0000002A: return "Hyper-V Server";
	case 0x0000002F: return "Starter N";
	case 0x00000030: return "Pro";
	case 0x00000031: return "Pro N";
	case 0x00000034: return "Server Solutions Premium";
	case 0x00000035: return "Server Solutions Premium (Core)";
	case 0x00000040: return "Server Hyper Core V";
	case 0x00000042: return "Starter E";
	case 0x00000043: return "Home Basic E";
	case 0x00000044: return "Premium E";
	case 0x00000045: return "Pro E";
	case 0x00000046: return "Enterprise E";
	case 0x00000047: return "Ultimate E";
	case 0x00000048: return "Enterprise (Eval)";
	case 0x0000004F: return "Server Standard (Eval)";
	case 0x00000050: return "Server Datacenter (Eval)";
	case 0x00000054: return "Enterprise N (Eval)";
	case 0x00000057: return "Thin PC";
	case 0x00000058: case 0x00000059: case 0x0000005A: case 0x0000005B: case 0x0000005C: return "Embedded";
	case 0x00000062: return "Home N";
	case 0x00000063: return "Home China";
	case 0x00000064: return "Home Single Language";
	case 0x00000065: return "Home";
	case 0x00000067: return "Pro with Media Center";
	case 0x00000069: case 0x0000006A: case 0x0000006B: case 0x0000006C: return "Embedded";
	case 0x0000006F: return "Home Connected";
	case 0x00000070: return "Pro Student";
	case 0x00000071: return "Home Connected N";
	case 0x00000072: return "Pro Student N";
	case 0x00000073: return "Home Connected Single Language";
	case 0x00000074: return "Home Connected China";
	case 0x00000079: return "Education";
	case 0x0000007A: return "Education N";
	case 0x0000007D: return "Enterprise LTSB";
	case 0x0000007E: return "Enterprise LTSB N";
	case 0x0000007F: return "Pro S";
	case 0x00000080: return "Pro S N";
	case 0x00000081: return "Enterprise LTSB (Eval)";
	case 0x00000082: return "Enterprise LTSB N (Eval)";
	case 0x0000008A: return "Pro Single Language";
	case 0x0000008B: return "Pro China";
	case 0x0000008C: return "Enterprise Subscription";
	case 0x0000008D: return "Enterprise Subscription N";
	case 0x00000091: return "Server Datacenter SA (Core)";
	case 0x00000092: return "Server Standard SA (Core)";
	case 0x00000095: return "Utility VM";
	case 0x000000A1: return "Pro for Workstations";
	case 0x000000A2: return "Pro for Workstations N";
	case 0x000000A4: return "Pro for Education";
	case 0x000000A5: return "Pro for Education N";
	case 0x000000AB: return "Enterprise G";	// I swear Microsoft are just making up editions...
	case 0x000000AC: return "Enterprise G N";
	case 0x000000B2: return "Cloud";
	case 0x000000B3: return "Cloud N";
	case 0x000000B6: return "Home OS";
	case 0x000000B7: case 0x000000CB: return "Cloud E";
	case 0x000000B9: return "IoT OS";
	case 0x000000BA: case 0x000000CA: return "Cloud E N";
	case 0x000000BB: return "IoT Edge OS";
	case 0x000000BC: return "IoT Enterprise";
	case 0x000000BD: return "Lite";
	case 0x000000BF: return "IoT Enterprise S";
	case 0x000000C0: case 0x000000C2: case 0x000000C3: case 0x000000C4: case 0x000000C5: case 0x000000C6: return "XBox";
	case 0x000000C7: case 0x000000C8: case 0x00000196: case 0x00000197: case 0x00000198: return "Azure Server";
	case 0xABCDABCD: return "(Unlicensed)";
	default:
		static_sprintf(unknown_edition_str, "(Unknown Edition 0x%02X)", (uint32_t)ProductType);
		return unknown_edition_str;
	}
}

PF_TYPE_DECL(WINAPI, HRESULT, WldpQueryWindowsLockdownMode, (PWLDP_WINDOWS_LOCKDOWN_MODE));
BOOL isSMode(void)
{
	BOOL r = FALSE;
	WLDP_WINDOWS_LOCKDOWN_MODE mode;
	PF_INIT_OR_OUT(WldpQueryWindowsLockdownMode, Wldp);

	HRESULT hr = pfWldpQueryWindowsLockdownMode(&mode);
	if (hr != S_OK) {
		SetLastError((DWORD)hr);
		uprintf("Could not detect S Mode: %s", WindowsErrorString());
	} else {
		r = (mode != WLDP_WINDOWS_LOCKDOWN_MODE_UNLOCKED);
	}

out:
	return r;
}

/*
 * Modified from smartmontools' os_win32.cpp
 */
void GetWindowsVersion(windows_version_t* windows_version)
{
	OSVERSIONINFOEXA vi, vi2;
	DWORD dwProductType = 0;
	const char* w = NULL;
	const char* arch_name;
	char *vptr;
	size_t vlen;
	DWORD major = 0, minor = 0;
	USHORT ProcessMachine = IMAGE_FILE_MACHINE_UNKNOWN, NativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
	ULONGLONG major_equal, minor_equal;
	BOOL ws, is_wow64 = FALSE;

	PF_TYPE_DECL(WINAPI, BOOL, IsWow64Process2, (HANDLE, USHORT*, USHORT*));
	PF_INIT(IsWow64Process2, Kernel32);

	memset(windows_version, 0, sizeof(windows_version_t));
	static_strcpy(windows_version->VersionStr, "Windows Undefined");

	memset(&vi, 0, sizeof(vi));
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (!GetVersionExA((OSVERSIONINFOA *)&vi)) {
		memset(&vi, 0, sizeof(vi));
		vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
		if (!GetVersionExA((OSVERSIONINFOA *)&vi))
			return;
	}

	if (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) {

		if (vi.dwMajorVersion > 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2)) {
			// Starting with Windows 8.1 Preview, GetVersionEx() does no longer report the actual OS version
			// See: http://msdn.microsoft.com/en-us/library/windows/desktop/dn302074.aspx
			// And starting with Windows 10 Preview 2, Windows enforces the use of the application/supportedOS
			// manifest in order for VerSetConditionMask() to report the ACTUAL OS major and minor...

			major_equal = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
			for (major = vi.dwMajorVersion; major <= 9; major++) {
				memset(&vi2, 0, sizeof(vi2));
				vi2.dwOSVersionInfoSize = sizeof(vi2); vi2.dwMajorVersion = major;
				if (!VerifyVersionInfoA(&vi2, VER_MAJORVERSION, major_equal))
					continue;
				if (vi.dwMajorVersion < major) {
					vi.dwMajorVersion = major; vi.dwMinorVersion = 0;
				}

				minor_equal = VerSetConditionMask(0, VER_MINORVERSION, VER_EQUAL);
				for (minor = vi.dwMinorVersion; minor <= 9; minor++) {
					memset(&vi2, 0, sizeof(vi2)); vi2.dwOSVersionInfoSize = sizeof(vi2);
					vi2.dwMinorVersion = minor;
					if (!VerifyVersionInfoA(&vi2, VER_MINORVERSION, minor_equal))
						continue;
					vi.dwMinorVersion = minor;
					break;
				}

				break;
			}
		}

		if (vi.dwMajorVersion <= 0xf && vi.dwMinorVersion <= 0xf) {
			ws = (vi.wProductType <= VER_NT_WORKSTATION);
			windows_version->Version = vi.dwMajorVersion << 4 | vi.dwMinorVersion;
			switch (windows_version->Version) {
			case WINDOWS_XP: w = "XP";
				break;
			case WINDOWS_2003: w = (ws ? "XP_64" : (!GetSystemMetrics(89) ? "Server 2003" : "Server 2003_R2"));
				break;
			case WINDOWS_VISTA: w = (ws ? "Vista" : "Server 2008");
				break;
			case WINDOWS_7: w = (ws ? "7" : "Server 2008_R2");
				break;
			case WINDOWS_8: w = (ws ? "8" : "Server 2012");
				break;
			case WINDOWS_8_1: w = (ws ? "8.1" : "Server 2012_R2");
				break;
			case WINDOWS_10_PREVIEW1: w = (ws ? "10 (Preview 1)" : "Server 10 (Preview 1)");
				break;
			// Starting with Windows 10 Preview 2, the major is the same as the public-facing version
			case WINDOWS_10:
				if (vi.dwBuildNumber < 20000) {
					w = (ws ? "10" : ((vi.dwBuildNumber < 17763) ? "Server 2016" : "Server 2019"));
					break;
				}
				windows_version->Version = WINDOWS_11;
				major = 11;
				// Fall through
			case WINDOWS_11: w = (ws ? "11" : "Server 2022");
				break;
			default:
				if (windows_version->Version < WINDOWS_XP)
					windows_version->Version = WINDOWS_UNDEFINED;
				else
					w = "12 or later";
				break;
			}
		}
	}
	windows_version->Major = major;
	windows_version->Minor = minor;

	if ((pfIsWow64Process2 != NULL) && pfIsWow64Process2(GetCurrentProcess(), &ProcessMachine, &NativeMachine)) {
		windows_version->Arch = NativeMachine;
	} else {
		// Assume same arch as the app
		windows_version->Arch = GetApplicationArch();
		// Fix the Arch if we have a 32-bit app running under WOW64
		if ((sizeof(uintptr_t) < 8) && IsWow64Process(GetCurrentProcess(), &is_wow64) && is_wow64) {
			if (windows_version->Arch == IMAGE_FILE_MACHINE_I386)
				windows_version->Arch = IMAGE_FILE_MACHINE_AMD64;
			else if (windows_version->Arch == IMAGE_FILE_MACHINE_ARM)
				windows_version->Arch = IMAGE_FILE_MACHINE_ARM64;
			else // I sure wanna be made aware of this scenario...
				assert(FALSE);
		}
		uprintf("Note: Underlying Windows architecture was guessed and may be incorrect...");
	}
	arch_name = GetArchName(windows_version->Arch);

	GetProductInfo(vi.dwMajorVersion, vi.dwMinorVersion, vi.wServicePackMajor, vi.wServicePackMinor, &dwProductType);
	vptr = &windows_version->VersionStr[sizeof("Windows ") - 1];
	vlen = sizeof(windows_version->VersionStr) - sizeof("Windows ") - 1;
	if (!w)
		safe_sprintf(vptr, vlen, "%s %u.%u %s", (vi.dwPlatformId == VER_PLATFORM_WIN32_NT ? "NT" : "??"),
			(unsigned)vi.dwMajorVersion, (unsigned)vi.dwMinorVersion, arch_name);
	else if (vi.wServicePackMinor)
		safe_sprintf(vptr, vlen, "%s SP%u.%u %s", w, vi.wServicePackMajor, vi.wServicePackMinor, arch_name);
	else if (vi.wServicePackMajor)
		safe_sprintf(vptr, vlen, "%s SP%u %s", w, vi.wServicePackMajor, arch_name);
	else
		safe_sprintf(vptr, vlen, "%s%s%s %s",
			w, (dwProductType != 0) ? " " : "", GetEdition(dwProductType), arch_name);

	windows_version->Edition = (int)dwProductType;

	// Add the build number (including UBR if available)
	windows_version->BuildNumber = vi.dwBuildNumber;
	windows_version->Ubr = ReadRegistryKey32(REGKEY_HKLM, "Software\\Microsoft\\Windows NT\\CurrentVersion\\UBR");
	vptr = &windows_version->VersionStr[safe_strlen(windows_version->VersionStr)];
	vlen = sizeof(windows_version->VersionStr) - safe_strlen(windows_version->VersionStr) - 1;
	if (windows_version->Ubr != 0)
		safe_sprintf(vptr, vlen, " (Build %lu.%lu)", windows_version->BuildNumber, windows_version->Ubr);
	else
		safe_sprintf(vptr, vlen, " (Build %lu)", windows_version->BuildNumber);
	vptr = &windows_version->VersionStr[safe_strlen(windows_version->VersionStr)];
	vlen = sizeof(windows_version->VersionStr) - safe_strlen(windows_version->VersionStr) - 1;
	if (isSMode())
		safe_sprintf(vptr, vlen, " in S Mode");
}

/*
 * Why oh why does Microsoft make it so convoluted to retrieve a measly executable's version number ?
 */
version_t* GetExecutableVersion(const char* path)
{
	static version_t version, *r = NULL;
	uint8_t* buf = NULL;
	UINT uLen;
	DWORD dwSize, dwHandle;
	VS_FIXEDFILEINFO* version_info;

	memset(&version, 0, sizeof(version));

	dwSize = GetFileVersionInfoSizeU(path, &dwHandle);
	if (dwSize == 0)
		goto out;

	buf = malloc(dwSize);
	if (buf == NULL)
		goto out;;
	if (!GetFileVersionInfoU(path, dwHandle, dwSize, buf))
		goto out;

	if (!VerQueryValueA(buf, "\\", (LPVOID*)&version_info, &uLen) || uLen == 0)
		goto out;

	if (version_info->dwSignature != 0xfeef04bd)
		goto out;

	version.Major = (version_info->dwFileVersionMS >> 16) & 0xffff;
	version.Minor = (version_info->dwFileVersionMS >> 0) & 0xffff;
	version.Micro = (version_info->dwFileVersionLS >> 16) & 0xffff;
	version.Nano = (version_info->dwFileVersionLS >> 0) & 0xffff;
	r = &version;

out:
	free(buf);
	return r;
}

/*
 * String array manipulation
 */
void StrArrayCreate(StrArray* arr, uint32_t initial_size)
{
	if (arr == NULL) return;
	arr->Max = initial_size; arr->Index = 0;
	arr->String = (char**)calloc(arr->Max, sizeof(char*));
	if (arr->String == NULL)
		uprintf("Could not allocate string array");
}

int32_t StrArrayAdd(StrArray* arr, const char* str, BOOL duplicate)
{
	char** old_table;
	if ((arr == NULL) || (arr->String == NULL) || (str == NULL))
		return -1;
	if (arr->Index == arr->Max) {
		arr->Max *= 2;
		old_table = arr->String;
		arr->String = (char**)realloc(arr->String, arr->Max*sizeof(char*));
		if (arr->String == NULL) {
			free(old_table);
			uprintf("Could not reallocate string array");
			return -1;
		}
	}
	arr->String[arr->Index] = (duplicate)?safe_strdup(str):(char*)str;
	if (arr->String[arr->Index] == NULL) {
		uprintf("Could not store string in array");
		return -1;
	}
	return arr->Index++;
}

int32_t StrArrayFind(StrArray* arr, const char* str)
{
	uint32_t i;
	if ((str == NULL) || (arr == NULL) || (arr->String == NULL))
		return -1;
	for (i = 0; i<arr->Index; i++) {
		if (strcmp(arr->String[i], str) == 0)
			return (int32_t)i;
	}
	return -1;
}

void StrArrayClear(StrArray* arr)
{
	uint32_t i;
	if ((arr == NULL) || (arr->String == NULL))
		return;
	for (i = 0; i < arr->Index; i++) {
		safe_free(arr->String[i]);
	}
	arr->Index = 0;
}

void StrArrayDestroy(StrArray* arr)
{
	StrArrayClear(arr);
	if (arr != NULL)
		safe_free(arr->String);
}

/*
 * Retrieve the SID of the current user. The returned PSID must be freed by the caller using LocalFree()
 */
static PSID GetSID(void) {
	TOKEN_USER* tu = NULL;
	DWORD len;
	HANDLE token;
	PSID ret = NULL;
	char* psid_string = NULL;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		uprintf("OpenProcessToken failed: %s", WindowsErrorString());
		return NULL;
	}

	if (!GetTokenInformation(token, TokenUser, tu, 0, &len)) {
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			uprintf("GetTokenInformation (pre) failed: %s", WindowsErrorString());
			return NULL;
		}
		tu = (TOKEN_USER*)calloc(1, len);
	}
	if (tu == NULL) {
		return NULL;
	}

	if (GetTokenInformation(token, TokenUser, tu, len, &len)) {
		/*
		 * now of course, the interesting thing is that if you return tu->User.Sid
		 * but free tu, the PSID pointer becomes invalid after a while.
		 * The workaround? Convert to string then back to PSID
		 */
		if (!ConvertSidToStringSidA(tu->User.Sid, &psid_string)) {
			uprintf("Unable to convert SID to string: %s", WindowsErrorString());
			ret = NULL;
		} else {
			if (!ConvertStringSidToSidA(psid_string, &ret)) {
				uprintf("Unable to convert string back to SID: %s", WindowsErrorString());
				ret = NULL;
			}
			// MUST use LocalFree()
			LocalFree(psid_string);
		}
	} else {
		ret = NULL;
		uprintf("GetTokenInformation (real) failed: %s", WindowsErrorString());
	}
	free(tu);
	return ret;
}

BOOL FileIO(enum file_io_type io_type, char* path, char** buffer, DWORD* size)
{
	SECURITY_ATTRIBUTES s_attr, *sa = NULL;
	SECURITY_DESCRIPTOR s_desc;
	const LARGE_INTEGER liZero = { .QuadPart = 0ULL };
	PSID sid = NULL;
	HANDLE handle;
	DWORD dwDesiredAccess = 0, dwCreationDisposition = 0;
	BOOL r = FALSE;
	BOOL ret = FALSE;

	// Change the owner from admin to regular user
	sid = GetSID();
	if ( (sid != NULL)
	  && InitializeSecurityDescriptor(&s_desc, SECURITY_DESCRIPTOR_REVISION)
	  && SetSecurityDescriptorOwner(&s_desc, sid, FALSE) ) {
		s_attr.nLength = sizeof(SECURITY_ATTRIBUTES);
		s_attr.bInheritHandle = FALSE;
		s_attr.lpSecurityDescriptor = &s_desc;
		sa = &s_attr;
	} else {
		uprintf("Could not set security descriptor: %s", WindowsErrorString());
	}

	switch (io_type) {
	case FILE_IO_READ:
		*buffer = NULL;
		dwDesiredAccess = GENERIC_READ;
		dwCreationDisposition = OPEN_EXISTING;
		break;
	case FILE_IO_WRITE:
		dwDesiredAccess = GENERIC_WRITE;
		dwCreationDisposition = CREATE_ALWAYS;
		break;
	case FILE_IO_APPEND:
		dwDesiredAccess = FILE_APPEND_DATA;
		dwCreationDisposition = OPEN_ALWAYS;
		break;
	default:
		assert(FALSE);
		break;
	}
	handle = CreateFileU(path, dwDesiredAccess, FILE_SHARE_READ, sa,
		dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not open '%s': %s", path, WindowsErrorString());
		goto out;
	}

	switch (io_type) {
	case FILE_IO_READ:
		*size = GetFileSize(handle, NULL);
		*buffer = (char*)malloc(*size);
		if (*buffer == NULL) {
			uprintf("Could not allocate buffer for reading file");
			goto out;
		}
		r = ReadFile(handle, *buffer, *size, size, NULL);
		break;
	case FILE_IO_APPEND:
		SetFilePointerEx(handle, liZero, NULL, FILE_END);
		// Fall through
	case FILE_IO_WRITE:
		r = WriteFile(handle, *buffer, *size, size, NULL);
		break;
	}

	if (!r) {
		uprintf("I/O Error: %s", WindowsErrorString());
		goto out;
	}

	PrintInfoDebug(0, (io_type == FILE_IO_READ) ? MSG_215 : MSG_216, path);
	ret = TRUE;

out:
	CloseHandle(handle);
	if (!ret && (io_type == FILE_IO_READ)) {
		// Only leave the buffer allocated if we were able to read data
		safe_free(*buffer);
		*size = 0;
	}
	return ret;
}

/*
 * Get a resource from the RC. If needed that resource can be duplicated.
 * If duplicate is true and len is non-zero, the a zeroed buffer of 'len'
 * size is allocated for the resource. Else the buffer is allocated for
 * the resource size.
 */
uint8_t* GetResource(HMODULE module, char* name, char* type, const char* desc, DWORD* len, BOOL duplicate)
{
	HGLOBAL res_handle;
	HRSRC res;
	DWORD res_len;
	uint8_t* p = NULL;

	res = FindResourceA(module, name, type);
	if (res == NULL) {
		uprintf("Could not locate resource '%s': %s", desc, WindowsErrorString());
		goto out;
	}
	res_handle = LoadResource(module, res);
	if (res_handle == NULL) {
		uprintf("Could not load resource '%s': %s", desc, WindowsErrorString());
		goto out;
	}
	res_len = SizeofResource(module, res);

	if (duplicate) {
		if (*len == 0)
			*len = res_len;
		p = calloc(*len, 1);
		if (p == NULL) {
			uprintf("Could not allocate resource '%s'", desc);
			goto out;
		}
		memcpy(p, LockResource(res_handle), min(res_len, *len));
		if (res_len > *len)
			uprintf("WARNING: Resource '%s' was truncated by %d bytes!", desc, res_len - *len);
	} else {
		p = LockResource(res_handle);
	}
	*len = res_len;

out:
	return p;
}

DWORD GetResourceSize(HMODULE module, char* name, char* type, const char* desc)
{
	DWORD len = 0;
	return (GetResource(module, name, type, desc, &len, FALSE) == NULL)?0:len;
}

// Run a console command, with optional redirection of stdout and stderr to our log
// as well as optional progress reporting if msg is not 0.
DWORD RunCommandWithProgress(const char* cmd, const char* dir, BOOL log, int msg)
{
	DWORD i, ret, dwRead, dwAvail, dwPipeSize = 4096;
	STARTUPINFOA si = { 0 };
	PROCESS_INFORMATION pi = { 0 };
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
	HANDLE hOutputRead = INVALID_HANDLE_VALUE, hOutputWrite = INVALID_HANDLE_VALUE;
	int match_length;
	static char* output;
	// For detecting typical dism.exe commandline progress report of type:
	// "\r[====                       8.0%                           ]\r\n"
	re_t pattern = re_compile("\\s*\\[[= ]+[\\d\\.]+%[= ]+\\]\\s*");

	si.cb = sizeof(si);
	if (log) {
		// NB: The size of a pipe is a suggestion, NOT an absolute guarantee
		// This means that you may get a pipe of 4K even if you requested 1K
		if (!CreatePipe(&hOutputRead, &hOutputWrite, &sa, dwPipeSize)) {
			ret = GetLastError();
			uprintf("Could not set commandline pipe: %s", WindowsErrorString());
			goto out;
		}
		si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES | STARTF_PREVENTPINNING | STARTF_TITLEISAPPID;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = hOutputWrite;
		si.hStdError = hOutputWrite;
	}

	if (!CreateProcessU(NULL, cmd, NULL, NULL, TRUE,
		NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
		ret = GetLastError();
		uprintf("Unable to launch command '%s': %s", cmd, WindowsErrorString());
		goto out;
	}

	if (log || msg != 0) {
		if (msg != 0)
			UpdateProgressWithInfoInit(NULL, FALSE);
		while (1) {
			// Check for user cancel
			if (IS_ERROR(ErrorStatus) && (SCODE_CODE(ErrorStatus) == ERROR_CANCELLED)) {
				if (!TerminateProcess(pi.hProcess, ERROR_CANCELLED)) {
					uprintf("Could not terminate command: %s", WindowsErrorString());
				} else switch (WaitForSingleObject(pi.hProcess, 5000)) {
				case WAIT_TIMEOUT:
					uprintf("Command did not terminate within timeout duration");
					break;
				case WAIT_OBJECT_0:
					uprintf("Command was terminated by user");
					break;
				default:
					uprintf("Error while waiting for command to be terminated: %s", WindowsErrorString());
					break;
				}
				ret = ERROR_CANCELLED;
				goto out;
			}
			// coverity[string_null]
			if (PeekNamedPipe(hOutputRead, NULL, dwPipeSize, NULL, &dwAvail, NULL)) {
				if (dwAvail != 0) {
					output = malloc(dwAvail + 1);
					if ((output != NULL) && (ReadFile(hOutputRead, output, dwAvail, &dwRead, NULL)) && (dwRead != 0)) {
						output[dwAvail] = 0;
						// Process a commandline progress bar into a percentage
						if ((msg != 0) && (re_matchp(pattern, output, &match_length) != -1)) {
							float f = 0.0f;
							i = 0;
next_progress_line:
							for (; (i < dwAvail) && (output[i] < '0' || output[i] > '9'); i++);
							IGNORE_RETVAL(sscanf(&output[i], "%f*", &f));
							UpdateProgressWithInfo(OP_FORMAT, msg, (uint64_t)(f * 100.0f), 100 * 100ULL);
							// Go to next line
							while ((++i < dwAvail) && (output[i] != '\n') && (output[i] != '\r'));
							while ((++i < dwAvail) && ((output[i] == '\n') || (output[i] == '\r')));
							// Print additional lines, if any
							if (i < dwAvail) {
								// Might have two consecutive progress lines in our buffer
								if (re_matchp(pattern, &output[i], &match_length) != -1)
									goto next_progress_line;
								uprintf("%s", &output[i]);
							}
						} else if (log) {
							// output may contain a '%' so don't feed it as a naked format string
							uprintf("%s", output);
						}
					}
					free(output);
				}
			}
			if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
				break;
			Sleep(100);
		};
	} else {
		// TODO: Detect user cancellation here?
		WaitForSingleObject(pi.hProcess, INFINITE);
	}

	if (!GetExitCodeProcess(pi.hProcess, &ret))
		ret = GetLastError();
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

out:
	safe_closehandle(hOutputWrite);
	safe_closehandle(hOutputRead);
	return ret;
}

BOOL CompareGUID(const GUID *guid1, const GUID *guid2) {
	if ((guid1 != NULL) && (guid2 != NULL)) {
		return (memcmp(guid1, guid2, sizeof(GUID)) == 0);
	}
	return FALSE;
}

static BOOL CALLBACK EnumFontFamExProc(const LOGFONTA *lpelfe,
	const TEXTMETRICA *lpntme, DWORD FontType, LPARAM lParam)
{
	return TRUE;
}

BOOL IsFontAvailable(const char* font_name)
{
	BOOL r;
	LOGFONTA lf = { 0 };
	HDC hDC = GetDC(hMainDialog);

	if (font_name == NULL) {
		safe_release_dc(hMainDialog, hDC);
		return FALSE;
	}

	lf.lfCharSet = DEFAULT_CHARSET;
	safe_strcpy(lf.lfFaceName, LF_FACESIZE, font_name);

	r = EnumFontFamiliesExA(hDC, &lf, EnumFontFamExProc, 0, 0);
	safe_release_dc(hMainDialog, hDC);
	return r;
}

/*
 * Set or restore a Local Group Policy DWORD key indexed by szPath/SzPolicy
 */
// I've seen rare cases where pLGPO->lpVtbl->Save(...) gets stuck, which prevents the
// application from launching altogether. To alleviate this, use a thread that we can
// terminate if needed...
typedef struct {
	BOOL bRestore;
	BOOL* bExistingKey;
	const char* szPath;
	const char* szPolicy;
	DWORD dwValue;
} SetLGP_Params;

DWORD WINAPI SetLGPThread(LPVOID param)
{
	SetLGP_Params* p = (SetLGP_Params*)param;
	LONG r;
	DWORD disp, regtype, val=0, val_size=sizeof(DWORD);
	HRESULT hr;
	IGroupPolicyObject* pLGPO;
	// Along with global 'existing_key', this static value is used to restore initial state
	static DWORD original_val;
	HKEY path_key = NULL, policy_key = NULL;
	// MSVC is finicky about these ones even if you link against gpedit.lib => redefine them
	const IID my_IID_IGroupPolicyObject =
		{ 0xea502723L, 0xa23d, 0x11d1, { 0xa7, 0xd3, 0x0, 0x0, 0xf8, 0x75, 0x71, 0xe3 } };
	const IID my_CLSID_GroupPolicyObject =
		{ 0xea502722L, 0xa23d, 0x11d1, { 0xa7, 0xd3, 0x0, 0x0, 0xf8, 0x75, 0x71, 0xe3 } };
	GUID ext_guid = REGISTRY_EXTENSION_GUID;
	// Can be anything really
	GUID snap_guid = { 0x3D271CFCL, 0x2BC6, 0x4AC2, {0xB6, 0x33, 0x3B, 0xDF, 0xF5, 0xBD, 0xAB, 0x2A} };

	// Reinitialize COM since it's not shared between threads
	IGNORE_RETVAL(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

	// We need an IGroupPolicyObject instance to set a Local Group Policy
	hr = CoCreateInstance(&my_CLSID_GroupPolicyObject, NULL, CLSCTX_INPROC_SERVER, &my_IID_IGroupPolicyObject, (LPVOID*)&pLGPO);
	if (FAILED(hr)) {
		ubprintf("SetLGP: CoCreateInstance failed; hr = %lx", hr);
		goto error;
	}

	hr = pLGPO->lpVtbl->OpenLocalMachineGPO(pLGPO, GPO_OPEN_LOAD_REGISTRY);
	if (FAILED(hr)) {
		ubprintf("SetLGP: OpenLocalMachineGPO failed - error %lx", hr);
		goto error;
	}

	hr = pLGPO->lpVtbl->GetRegistryKey(pLGPO, GPO_SECTION_MACHINE, &path_key);
	if (FAILED(hr)) {
		ubprintf("SetLGP: GetRegistryKey failed - error %lx", hr);
		goto error;
	}

	r = RegCreateKeyExA(path_key, p->szPath, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
		NULL, &policy_key, &disp);
	if (r != ERROR_SUCCESS) {
		ubprintf("SetLGP: Failed to open LGPO path %s - error %lx", p->szPath, hr);
		policy_key = NULL;
		goto error;
	}

	if ((disp == REG_OPENED_EXISTING_KEY) && (!p->bRestore) && (!(*(p->bExistingKey)))) {
		// backup existing value for restore
		*(p->bExistingKey) = TRUE;
		regtype = REG_DWORD;
		r = RegQueryValueExA(policy_key, p->szPolicy, NULL, &regtype, (LPBYTE)&original_val, &val_size);
		if (r == ERROR_FILE_NOT_FOUND) {
			// The Key exists but not its value, which is OK
			*(p->bExistingKey) = FALSE;
		} else if (r != ERROR_SUCCESS) {
			ubprintf("SetLGP: Failed to read original %s policy value - error %lx", p->szPolicy, r);
		}
	}

	if ((!p->bRestore) || (*(p->bExistingKey))) {
		val = (p->bRestore)?original_val:p->dwValue;
		r = RegSetValueExA(policy_key, p->szPolicy, 0, REG_DWORD, (BYTE*)&val, sizeof(val));
	} else {
		r = RegDeleteValueA(policy_key, p->szPolicy);
	}
	if (r != ERROR_SUCCESS) {
		ubprintf("SetLGP: RegSetValueEx / RegDeleteValue failed - error %lx", r);
	}
	RegCloseKey(policy_key);
	policy_key = NULL;

	// Apply policy
	hr = pLGPO->lpVtbl->Save(pLGPO, TRUE, (p->bRestore)?FALSE:TRUE, &ext_guid, &snap_guid);
	if (hr != S_OK) {
		ubprintf("SetLGP: Unable to apply %s policy - error %lx", p->szPolicy, hr);
		goto error;
	} else {
		if ((!p->bRestore) || (*(p->bExistingKey))) {
			ubprintf("SetLGP: Successfully %s %s policy to 0x%08lX", (p->bRestore)?"restored":"set", p->szPolicy, val);
		} else {
			ubprintf("SetLGP: Successfully removed %s policy key", p->szPolicy);
		}
	}

	RegCloseKey(path_key);
	pLGPO->lpVtbl->Release(pLGPO);
	return TRUE;

error:
	if (path_key != NULL)
		RegCloseKey(path_key);
	if (pLGPO != NULL)
		pLGPO->lpVtbl->Release(pLGPO);
	CoUninitialize();
	return FALSE;
}

BOOL SetLGP(BOOL bRestore, BOOL* bExistingKey, const char* szPath, const char* szPolicy, DWORD dwValue)
{
	SetLGP_Params params = {bRestore, bExistingKey, szPath, szPolicy, dwValue};
	DWORD r = FALSE;
	HANDLE thread_id;

	if (ReadSettingBool(SETTING_DISABLE_LGP)) {
		ubprintf("LPG handling disabled, per settings");
		return FALSE;
	}

	thread_id = CreateThread(NULL, 0, SetLGPThread, (LPVOID)&params, 0, NULL);
	if (thread_id == NULL) {
		ubprintf("SetLGP: Unable to start thread");
		return FALSE;
	}
	if (WaitForSingleObject(thread_id, 5000) != WAIT_OBJECT_0) {
		ubprintf("SetLGP: Killing stuck thread!");
		TerminateThread(thread_id, 0);
		CloseHandle(thread_id);
		return FALSE;
	}
	if (!GetExitCodeThread(thread_id, &r))
		return FALSE;
	return (BOOL) r;
}

/*
 * This call tries to evenly balance the affinities for an array of
 * num_threads, according to the number of cores at our disposal...
 */
BOOL SetThreadAffinity(DWORD_PTR* thread_affinity, size_t num_threads)
{
	size_t i, j, pc;
	DWORD_PTR affinity, dummy;

	memset(thread_affinity, 0, num_threads * sizeof(DWORD_PTR));
	if (!GetProcessAffinityMask(GetCurrentProcess(), &affinity, &dummy))
		return FALSE;
	uuprintf("\r\nThread affinities:");
	uuprintf("  avail:\t%s", printbitslz(affinity));

	// If we don't have enough virtual cores to evenly spread our load forget it
	pc = popcnt64(affinity);
	if (pc < num_threads)
		return FALSE;

	// Spread the affinity as evenly as we can
	thread_affinity[num_threads - 1] = affinity;
	for (i = 0; i < num_threads - 1; i++) {
		for (j = 0; j < pc / num_threads; j++) {
			thread_affinity[i] |= affinity & (-1LL * affinity);
			affinity ^= affinity & (-1LL * affinity);
		}
		uuprintf("  thr_%d:\t%s", i, printbitslz(thread_affinity[i]));
		thread_affinity[num_threads - 1] ^= thread_affinity[i];
	}
	uuprintf("  thr_%d:\t%s", i, printbitslz(thread_affinity[i]));
	return TRUE;
}

/*
 * Returns true if:
 * 1. The OS supports UAC, UAC is on, and the current process runs elevated, or
 * 2. The OS doesn't support UAC or UAC is off, and the process is being run by a member of the admin group
 */
BOOL IsCurrentProcessElevated(void)
{
	BOOL r = FALSE;
	DWORD size;
	HANDLE token = INVALID_HANDLE_VALUE;
	TOKEN_ELEVATION te;
	SID_IDENTIFIER_AUTHORITY auth = { SECURITY_NT_AUTHORITY };
	PSID psid;

	if (ReadRegistryKey32(REGKEY_HKLM, "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\\EnableLUA") == 1) {
		uprintf("Note: UAC is active");
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
			uprintf("Could not get current process token: %s", WindowsErrorString());
			goto out;
		}
		if (!GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size)) {
			uprintf("Could not get token information: %s", WindowsErrorString());
			goto out;
		}
		r = (te.TokenIsElevated != 0);
	} else {
		uprintf("Note: UAC is either disabled or not available");
		if (!AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
			DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &psid))
			goto out;
		if (!CheckTokenMembership(NULL, psid, &r))
			r = FALSE;
		FreeSid(psid);
	}

out:
	safe_closehandle(token);
	return r;
}

char* ToLocaleName(DWORD lang_id)
{
	static char mui_str[LOCALE_NAME_MAX_LENGTH];
	wchar_t wmui_str[LOCALE_NAME_MAX_LENGTH];

	if (LCIDToLocaleName(lang_id, wmui_str, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
		wchar_to_utf8_no_alloc(wmui_str, mui_str, LOCALE_NAME_MAX_LENGTH);
	} else {
		static_strcpy(mui_str, "en-US");
	}
	return mui_str;
}

/*
 * From: https://stackoverflow.com/a/40390858/1069307
 */
BOOL SetPrivilege(HANDLE hToken, LPCWSTR pwzPrivilegeName, BOOL bEnable)
{
	TOKEN_PRIVILEGES tp;
	LUID luid;

	if (!LookupPrivilegeValue(NULL, pwzPrivilegeName, &luid)) {
		uprintf("Could not lookup '%S' privilege: %s", pwzPrivilegeName, WindowsErrorString());
		return FALSE;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = bEnable ? SE_PRIVILEGE_ENABLED : 0;

	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
		uprintf("Could not %s '%S' privilege: %s",
			bEnable ? "enable" : "disable", pwzPrivilegeName, WindowsErrorString());
		return FALSE;
	}

	if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
		uprintf("Error assigning privileges: %s", WindowsErrorString());
		return FALSE;
	}

	return TRUE;
}

/*
 * Mount an offline registry hive located at <pszHivePath> into <key>\<pszHiveName>.
 * <key> should be HKEY_LOCAL_MACHINE or HKEY_USERS.
 */
BOOL MountRegistryHive(const HKEY key, const char* pszHiveName, const char* pszHivePath)
{
	LSTATUS status;
	HANDLE token = INVALID_HANDLE_VALUE;

	if_not_assert((key == HKEY_LOCAL_MACHINE) || (key == HKEY_USERS))
		return FALSE;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token)) {
		uprintf("Could not get current process token: %s", WindowsErrorString());
		return FALSE;
	}

	// Ignore errors on those in case we can proceed without...
	SetPrivilege(token, SE_RESTORE_NAME, TRUE);
	SetPrivilege(token, SE_BACKUP_NAME, TRUE);

	status = RegLoadKeyA(key, pszHiveName, pszHivePath);
	if (status != ERROR_SUCCESS) {
		SetLastError(status);
		uprintf("Could not mount offline registry hive '%s': %s", pszHivePath, WindowsErrorString());
	} else
		uprintf("Mounted offline registry hive '%s' to '%s\\%s'",
			pszHivePath, (key == HKEY_LOCAL_MACHINE) ? "HKLM" : "HKCU", pszHiveName);

	safe_closehandle(token);
	return (status == ERROR_SUCCESS);
}

/*
 * Unmount an offline registry hive.
 * <key> should be HKEY_LOCAL_MACHINE or HKEY_USERS.
 */
BOOL UnmountRegistryHive(const HKEY key, const char* pszHiveName)
{
	LSTATUS status;

	if_not_assert((key == HKEY_LOCAL_MACHINE) || (key == HKEY_USERS))
		return FALSE;

	status = RegUnLoadKeyA(key, pszHiveName);
	if (status != ERROR_SUCCESS) {
		SetLastError(status);
		uprintf("Could not unmount offline registry hive: %s", WindowsErrorString());
	} else
		uprintf("Unmounted offline registry hive '%s\\%s'",
			(key == HKEY_LOCAL_MACHINE) ? "HKLM" : "HKCU", pszHiveName);

	return (status == ERROR_SUCCESS);
}

/*
 * Take administrative ownership of a file or directory, and grant all access rights.
 */
BOOL TakeOwnership(LPCSTR lpszOwnFile)
{
	BOOL ret = FALSE;
	HANDLE hToken = NULL;
	PSID pSIDAdmin = NULL;
	PACL pOldDACL = NULL, pNewDACL = NULL;
	PSECURITY_DESCRIPTOR pSD = NULL;
	SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
	EXPLICIT_ACCESS ea = { 0 };

	if (lpszOwnFile == NULL)
		return FALSE;

	// Create a SID for the BUILTIN\Administrators group.
	if (!AllocateAndInitializeSid(&SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID,
		DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pSIDAdmin))
		goto out;

	// Open a handle to the access token for the calling process.
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
		goto out;

	// Enable the SE_TAKE_OWNERSHIP_NAME privilege.
	if (!SetPrivilege(hToken, SE_TAKE_OWNERSHIP_NAME, TRUE))
		goto out;

	// Set the owner in the object's security descriptor.
	if (SetNamedSecurityInfoU(lpszOwnFile, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
		pSIDAdmin, NULL, NULL, NULL) != ERROR_SUCCESS)
		goto out;

	// Disable the SE_TAKE_OWNERSHIP_NAME privilege.
	if (!SetPrivilege(hToken, SE_TAKE_OWNERSHIP_NAME, FALSE))
		goto out;

	// Get a pointer to the existing DACL.
	if (GetNamedSecurityInfoU(lpszOwnFile, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
		NULL, NULL, &pOldDACL, NULL, &pSD) != ERROR_SUCCESS)
		goto out;

	// Initialize an EXPLICIT_ACCESS structure for the new ACE
	// with full control for Administrators.
	ea.grfAccessPermissions = GENERIC_ALL;
	ea.grfAccessMode = GRANT_ACCESS;
	ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea.Trustee.ptstrName = (LPTSTR)pSIDAdmin;

	// Create a new ACL that merges the new ACE into the existing DACL.
	if (SetEntriesInAcl(1, &ea, pOldDACL, &pNewDACL) != ERROR_SUCCESS)
		goto out;

	// Try to modify the object's DACL.
	if (SetNamedSecurityInfoU(lpszOwnFile, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
		NULL, NULL, pNewDACL, NULL) != ERROR_SUCCESS)
		goto out;

	ret = TRUE;

out:
	FreeSid(pSIDAdmin);
	LocalFree(pNewDACL);
	safe_closehandle(hToken);
	return ret;
}
