/* This software is distributed under the following license:
 * http://host-sflow.sourceforge.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include "util.h"
#include <io.h>
#include <time.h>

extern int debug;
extern FILE *logFile;

#define UT_DEFAULT_MAX_STRLEN 65535

/*_________________---------------------------__________________
  _________________        logging            __________________
  -----------------___________________________------------------
*/

void myLog(int syslogType, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	if (syslogType <= debug) {
		char datebuf[9];
		char timebuf[9];
		_strdate_s(datebuf, 9);
		_strtime_s(timebuf, 9);
		fprintf(logFile, "%s %s: ", datebuf, timebuf);
		vfprintf(logFile, fmt, args);
		fprintf(logFile, "\n");
		fflush(logFile);
	}
}

/*_________________---------------------------__________________
  _________________      truncateOpenFile     __________________
  -----------------___________________________------------------
*/
  
BOOL truncateOpenFile(FILE *fptr)
{
	int fd = _fileno(fptr);
	if (fd == -1) {
		myLog(LOG_ERR, "truncateOpenFile: _fileno() failed fd=%u", fd);
		return FALSE;
	}
	long pos = _lseek(fd, 0, SEEK_CUR);
	if (_chsize(fd, pos) != 0) {
		myLog(LOG_ERR, "truncateOpenFile: _chsize() failed");
		return FALSE;
	}
	return TRUE;
}

/*_________________---------------------------__________________
  _________________       my_os_allocation    __________________
  -----------------___________________________------------------
*/

void *my_os_calloc(size_t bytes)
{
	myLog(LOG_INFO, "my_os_calloc(%u)", bytes);
	void *mem = SYS_CALLOC(1, bytes);
	if (mem == NULL) {
		myLog(LOG_ERR, "calloc() failed : %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return mem;
}

void *my_os_realloc(void *ptr, size_t bytes)
{
	myLog(LOG_INFO, "my_os_realloc(%u)", bytes);
	void *mem = SYS_REALLOC(ptr, bytes);
	if (mem == NULL) {
		myLog(LOG_ERR, "realloc() failed : %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	return mem;
}
  
void my_os_free(void *ptr)
{
	if (ptr) {
		SYS_FREE(ptr);
	}
}

/*_________________---------------------------------------__________________
  _________________  Realm allocation (buffer recycling)  __________________
  -----------------_______________________________________------------------
*/

typedef union _UTHeapHeader {
	uint64_t hdrBits64[2];     // force sizeof(UTBufferHeader) == 128bits to ensure alignment
	union _UTHeapHeader *nxt;  // valid when in linked list waiting to be reallocated
	struct {                   // valid when buffer being used - store bookkeeping info here
		uint32_t realmIdx;
		uint16_t refCount;
#define UT_MAX_REFCOUNT 0xFFFF
		uint16_t queueIdx;
	} h;
} UTHeapHeader;

static UTHeapHeader *UTHeapQHdr(void *buf) 
{
	return (UTHeapHeader *)buf - 1;
}
 
typedef struct _UTHeapRealm {
#define UT_MAX_BUFFER_Q 32
	UTHeapHeader *bufferLists[UT_MAX_BUFFER_Q];
	uint32_t realmIdx;
	uint32_t totalAllocatedBytes;
} UTHeapRealm;

// separate realm for each thread
static __declspec(thread) UTHeapRealm utRealm;
  
static uint32_t UTHeapQSize(void *buf) 
{
	UTHeapHeader *utBuf = UTHeapQHdr(buf);
	return (1 << utBuf->h.queueIdx) - (uint32_t)sizeof(UTHeapHeader);
}

/*_________________---------------------------__________________
  _________________         UTHeapQNew        __________________
  -----------------___________________________------------------
  Variable-length, recyclable
*/

void *UTHeapQNew(size_t len) 
{
	// initialize the realm so that we can trap on any cross-thread
	// allocation activity.
	if (utRealm.realmIdx == 0) {
	// utRealm.realmIdx = MYGETTID; $$$
	}
	// take it up to the nearest power of 2, including room for my header
	// but make sure it is at least 16 bytes (queue 4), so we always have
	// 128-bit alignment (just in case it is needed)
	int queueIdx = 4;
	for (size_t l = (len + 15) >> 4; l > 0; l >>= 1) {
		queueIdx++;
	}
	UTHeapHeader *utBuf = (UTHeapHeader *)utRealm.bufferLists[queueIdx];
	if (utBuf) {
		// peel it off
		utRealm.bufferLists[queueIdx] = utBuf->nxt;
	} else {
		// allocate a new one
		utBuf = (UTHeapHeader *)my_os_calloc(1<<queueIdx);
		utRealm.totalAllocatedBytes += (1<<queueIdx);
	}
	// remember the details so we know what to do on free (overwriting the nxt pointer)
	utBuf->h.realmIdx = utRealm.realmIdx;
	utBuf->h.refCount = 1;
	utBuf->h.queueIdx = queueIdx;
	// return a pointer to just after the header
	return (char *)utBuf + sizeof(UTHeapHeader);
}

/*_________________---------------------------__________________
  _________________    UTHeapQFree            __________________
  -----------------___________________________------------------
*/

void UTHeapQFree(void *buf)
{
	UTHeapHeader *utBuf = UTHeapQHdr(buf);
	int rc = utBuf->h.refCount;
	assert(rc != 0);
	assert(utBuf->h.realmIdx == utRealm.realmIdx);

	// UT_MAX_REFCOUNT => immortality
	if (rc != UT_MAX_REFCOUNT) {
		// decrement the ref count
		if (--rc != 0) {
			// not zero yet, so just write back the decremented refcount
			utBuf->h.refCount = rc;
		} else {
			// reference count reached zero, so it's time to free this buffer for real
			// read the queue index before we overwrite it
			uint16_t queueIdx = utBuf->h.queueIdx;
			memset(utBuf, 0, 1 << queueIdx);
			// put it back on the queue
			utBuf->nxt = (UTHeapHeader *)(utRealm.bufferLists[queueIdx]);
			utRealm.bufferLists[queueIdx] = utBuf;
		}
	}
}

/*_________________---------------------------__________________
  _________________      UTHeapQReAlloc       __________________
  -----------------___________________________------------------
*/

void *UTHeapQReAlloc(void *buf, size_t newSiz)
{
	size_t siz = UTHeapQSize(buf);
	if (newSiz <= siz) {
		return buf;
	}
	void *newBuf = UTHeapQNew(newSiz);
	memcpy(newBuf, buf, siz);
	UTHeapQFree(buf);
	return newBuf;
}

/*_________________---------------------------__________________
  _________________      UTHeapQKeep          __________________
  -----------------___________________________------------------
*/

void UTHeapQKeep(void *buf)
{
	// might even need to grab the semaphore for this operation too?
	UTHeapHeader *utBuf = UTHeapQHdr(buf);
	assert(utBuf->h.refCount > 0);
	assert(utBuf->h.realmIdx == utRealm.realmIdx);
	if (++utBuf->h.refCount == 0) {
		utBuf->h.refCount = UT_MAX_REFCOUNT;
	}
}

/*________________---------------------------__________________
  _________________      UTHeapQTotal         __________________
  -----------------___________________________------------------
*/

uint64_t UTHeapQTotal(void)
{
	return utRealm.totalAllocatedBytes;
}

/*_________________---------------------------__________________
  _________________     string copy fns       __________________
  -----------------___________________________------------------
*/
  
/**
 * Allocates space for a new single byte character string on the heap using
 * my_calloc, copies the str to the new string and returns a pointer 
 * to the new string.
 */
char *my_strdup(char *str)
{
    if (str == NULL) {
		return NULL;
	}
	size_t len = strnlen_s(str, UT_DEFAULT_MAX_STRLEN);
    char *newStr = (char *)my_calloc(sizeof(char)*(len+1));
    memcpy(newStr, str, len);
    return newStr;
}

/**
 * Allocates space for a new wide character string on the heap using
 * my_calloc, copies the str to the new string and returns a pointer 
 * to the new string.
 */
wchar_t *my_wcsdup(wchar_t *str)
{
	if (str == NULL) {
		return NULL;
	}
	size_t len = wcsnlen_s(str, UT_DEFAULT_MAX_STRLEN) * sizeof(wchar_t); //length without null
	len += sizeof(wchar_t);
	wchar_t *newStr = (wchar_t *)my_calloc(len);
	memcpy(newStr, str, len);
	return newStr;
}

/**
 * Allocates space for a single byte character string on the heap using my_calloc, 
 * copies wcstr to the new string converting each character to a multi-byte character. 
 * Returns a pointer to the new single byte character string.
 * If wscstr contains characters that cannot be represented using a single byte character,
 * then the resulting string will be truncated.
 */
char *my_wcstombs(wchar_t *wcstr)
{
	size_t wcslen = 1+wcsnlen_s(wcstr, UT_DEFAULT_MAX_STRLEN);
	char *str = (char *)my_calloc(wcslen * sizeof(char));
	size_t numConverted;
	wcstombs_s(&numConverted, str, wcslen, wcstr, wcslen);
	return str;
}
     
/*_____________---------------------------------------________________
  _____________ Wide character string array functions ________________
  -------------_______________________________________----------------
*/
WcsArray *wcsArrayNew()
{
	return (WcsArray *)my_calloc(sizeof(WcsArray));
}

void wcsArrayAdd(WcsArray *wcsArray, wchar_t *str)
{
	wcsArray->sorted = false;
	if (wcsArray->capacity <= wcsArray->n) {
		uint32_t oldBytes = wcsArray->capacity * sizeof(wchar_t *);
		wcsArray->capacity = wcsArray->n + 16;
		uint32_t newBytes = wcsArray->capacity * sizeof(wchar_t *);
		wchar_t **newArray = (wchar_t **)my_calloc(newBytes);
		if (wcsArray->strings != NULL) {
			memcpy(newArray, wcsArray->strings, oldBytes);
			my_free(wcsArray->strings);
		}
		wcsArray->strings = newArray;
	}
	if (wcsArray->strings[wcsArray->n] != NULL) {
		my_free(wcsArray->strings[wcsArray->n]);
	}
	wcsArray->strings[wcsArray->n++] = my_wcsdup(str);
}

void wcsArrayReset(WcsArray *wcsArray)
{
	wcsArray->sorted = false;
	for (uint32_t i = 0; i < wcsArray->n; i++) {
		if (wcsArray->strings[i] != NULL) {
			my_free(wcsArray->strings[i]);
			wcsArray->strings[i] = NULL;
		}
	}
	wcsArray->n = 0;
}

void wcsArrayFree(WcsArray *wcsArray)
{
	wcsArrayReset(wcsArray);
	if (wcsArray->strings != NULL) {
		my_free(wcsArray->strings);
	}
	my_free(wcsArray);
}

uint32_t wcsArrayIndexOf(WcsArray *wcsArray, wchar_t * str)
{
	for (uint32_t i = 0; i < wcsArray->n; i++) {
		wchar_t * instr = wcsArray->strings[i];
		if (instr == str) {
			return i;
		}
		if (str && instr && wcscmp(str, instr) == 0) {
			return i;
		}
	}
	return -1;
}


/*________________---------------------------__________________
  ________________     hex2bin, bin2hex      __________________
  ----------------___________________________------------------
*/

static u_char hex2bin(u_char c)
{
	return (isdigit(c) ? (c)-'0': ((toupper(c))-'A')+10)  & 0xf;
}
  

static u_char bin2hex(int nib)
{
	return (nib < 10) ? ('0' + nib) : ('A' - 10 + nib);
}

static u_char whex2bin(wchar_t c)
{
	return (iswdigit(c) ? (c)-L'0' : ((towupper(c))-L'A')+10) & 0xf;
}

/*_________________---------------------------__________________
  _________________   printHex, hexToBinary   __________________
  -----------------___________________________------------------
*/

int printHex(const u_char *a, int len, u_char *buf, int bufLen, BOOL prefix)
{
	int b = 0;
	if (prefix) {
		buf[b++] = '0';
		buf[b++] = 'x';
	}
	for (int i = 0; i < len; i++) {
		if (b > (bufLen - 2)) {
			 // must be room for 2 characters
			return 0;
		}
		u_char byte = a[i];
		buf[b++] = bin2hex(byte >> 4);
		buf[b++] = bin2hex(byte & 0x0f);
	}

	// add NUL termination
	buf[b] = '\0';
	return b;
}
  
int hexToBinary(u_char *hex, u_char *bin, uint32_t binLen)
{
	// read from hex into bin, up to max binLen chars, return number written
	u_char *h = hex;
	u_char *b = bin;
	u_char c;
	uint32_t i = 0;
    while ((c = *h++) != '\0') {
		if (isxdigit(c)) {
			u_char val = hex2bin(c);
			if (isxdigit(*h)) {
				c = *h++;
				val = (val << 4) | hex2bin(c);
			}
			*b++ = val;
			if (++i >= binLen) {
				return i;
			}
		} else if (c != '.' && c != '-' && c != ':') { // allow a variety of byte-separators
			return i;
		}
	}
	return i;
}

/**
 * Reads up to len hex chars from wchar_t *hex, skipping over separators (. - : { }), 
 * and writes each pair of hex character as the binary representation in *bin.
 * Reads up to length hex characters, fewer if a null or non-hex non-separator 
 * char is found before length chars.
 * Returns the number of hex characters.
 */
int wchexToBinary(wchar_t *hex, u_char *bin, uint32_t len)
{
	uint32_t hi = 0;
	uint32_t bi = 0;
	wchar_t currChar;
	uint32_t count = 0;

	while ((currChar = hex[hi++]) != L'\0') {
		if (iswxdigit(currChar)) {
			UCHAR val = whex2bin(currChar);
			if (iswxdigit(hex[hi])) {
				currChar = hex[hi++];
				val = (val << 4) | whex2bin(currChar);
			}
			bin[bi++] = val;
			if (++count >= len) {
				return count;
			}
		} else if (currChar != L'.' && currChar != L'-' && currChar != L':'
			&& currChar != L'{' && currChar != L'}') { //allow a variety of byte separators and {}
			return count;
		}
	}
	return count;
}

/*_________________---------------------------__________________
  _________________   parseUUID, printUUID    __________________
  -----------------___________________________------------------
*/

BOOL parseUUID(char *str, char *uuid)
{
	if (hexToBinary((u_char *)str, (u_char *)uuid, 16) != 16) {
		return FALSE;
	}
	return TRUE;
}

  
int printUUID(const u_char *a, u_char *buf, int bufLen)
{
	int b = 0;
	b += printHex(a, 4, buf, bufLen, FALSE);
	buf[b++] = '-';
	b += printHex(a + 4, 2, buf + b, bufLen - b, FALSE);
	buf[b++] = '-';
	b += printHex(a + 6, 2, buf + b, bufLen - b, FALSE);
	buf[b++] = '-';
	b += printHex(a + 8, 2, buf + b, bufLen - b, FALSE);
	buf[b++] = '-';
	b += printHex(a + 10, 6, buf + b, bufLen - b, FALSE);
    
	// should really be lowercase hex - fix that here
	for (int i = 0; i < b; i++) buf[i] = tolower(buf[i]);

	// add NUL termination
	buf[b] = '\0';
	return b;
}

/**
 * Converts a wide character representation of a GUID (may include
 * separators) to a formatted GUID of single byte (lower case) characters.
 * Returns TRUE if the input guid is valid, FALSE otherwise.
 */
BOOL guidToString(wchar_t *guid, u_char *guidStr, int guidStrLen)
{
	UCHAR binGuid[16];
	if (wchexToBinary(guid, binGuid, 33) != 16) {
		return FALSE;
	}
	printUUID(binGuid, guidStr, guidStrLen);
	return FALSE;
}

/*________________---------------------------__________________
  ________________     WMI functions         __________________
  ----------------___________________________------------------
*/

/**
 * Connect to WMI using the specified path to the name space.
 * BSTR path containing path to the name space.
 * IWbemServices *pNamespace  pointer successfully connected to.
 * Returns HRESULT indicating success or not.
 */
HRESULT connectToWMI(BSTR path, IWbemServices **pNamespace)
{
	HRESULT hr = S_FALSE;
	IWbemLocator *pLocator = NULL;
	hr =  CoInitializeEx(0, COINIT_MULTITHREADED);
	if (!SUCCEEDED(hr)) {
		myLog(LOG_ERR,"connectToWMI failed to initialize COM");
		CoUninitialize();
		return hr;
	}

	hr =  CoInitializeSecurity(NULL,-1,NULL,NULL,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,NULL,EOAC_NONE,NULL);
	hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&pLocator );
	if (!SUCCEEDED(hr)) {
		myLog(LOG_ERR,"connectToWMI: failed to create WMI instance");
		CoUninitialize();
		return hr;
	}

	hr = pLocator->ConnectServer(path, NULL, NULL, NULL, 0, NULL, NULL, pNamespace);
	pLocator->Release();
	if (WBEM_S_NO_ERROR != hr) {
		myLog(LOG_ERR,"connecttoWMI: ConnectServer() failed for namespace %S", path);
		CoUninitialize();
	}
	return hr;
}

/**
 * Executes a WMI query against the IWbemServices namespace to find and return the endpoints
 * associated with the the classObj. The endpoints are associated via the assocClass, 
 * are of class endClass and play resultRole property in the association.
 * assocClass, endClass and resultRole are all mandatory.
 * The endpoints are returned with resultEnum.
 */
HRESULT associatorsOf(IWbemServices *pNamespace, IWbemClassObject *classObj, 
					  wchar_t *assocClass, wchar_t *resultClass, wchar_t *resultRole,
					  IEnumWbemClassObject **resultEnum)
{
	wchar_t *formatString = L"ASSOCIATORS OF {%s} WHERE AssocClass=%s ResultClass=%s ResultRole=%s";
	HRESULT hr;
	VARIANT path;
	hr = classObj->Get(L"__PATH", 0, &path, 0, 0);
	if (!SUCCEEDED(hr)) {
		return hr;
	}
	size_t length = wcslen(formatString)+SysStringLen(path.bstrVal)+
		wcslen(assocClass)+wcslen(resultClass)+wcslen(resultRole)+1;
	wchar_t *query = (wchar_t *)my_calloc(length*sizeof(wchar_t));
	swprintf_s(query, length, formatString, path.bstrVal, assocClass, resultClass, resultRole);
	hr = pNamespace->ExecQuery(L"WQL", query, WBEM_FLAG_FORWARD_ONLY, NULL, resultEnum);
	my_free(query);
	return hr;
}

/**
 * Replaces (in place) reservered characters to generate a counter
 * instance name.
 */

static wchar_t cleanCounterNameChar(wchar_t ch_in)
{
	wchar_t ch_out = ch_in;
	switch(ch_in) {
		case L'\\': ch_out = L'-'; break;
		case L'(': ch_out = L'['; break;
		case L')': ch_out = L']'; break;
		case L'#': ch_out = L'_'; break;
		case L'*': ch_out = L'_'; break;
		case L'/': ch_out = L'_'; break;
		default: break;
	}
	return ch_out;
}

void cleanCounterName(wchar_t *name) 
{
	// replace reserved characters to generate counter instance name
	size_t len = wcsnlen_s(name, UT_DEFAULT_MAX_STRLEN);
	for (uint32_t i = 0; i <= len; i++ ) {
		name[i] = cleanCounterNameChar(name[i]);
	}
}

 BOOL cleanCounterNameEqual(wchar_t *name1, wchar_t *name2) 
{
	// ignore reserved characters that get substituted by the OS
	size_t len1 = wcsnlen_s(name1, UT_DEFAULT_MAX_STRLEN);
	size_t len2 = wcsnlen_s(name2, UT_DEFAULT_MAX_STRLEN);
	if(len1 != len2) return FALSE;
	for (uint32_t i = 0; i <= len1; i++ ) {
		wchar_t ch1 = cleanCounterNameChar(name1[i]);
		wchar_t ch2 = cleanCounterNameChar(name2[i]);
		if(ch1 != ch2) return FALSE;
	}
	return TRUE;
}
 
 
/*________________---------------------------__________________
  ________________      adaptorList          __________________
  ----------------___________________________------------------
*/

SFLAdaptorList *adaptorListNew()
{
	SFLAdaptorList *adList = (SFLAdaptorList *)my_calloc(sizeof(SFLAdaptorList));
	adList->capacity = 2; // will grow if necessary
	adList->adaptors = (SFLAdaptor **)my_calloc(adList->capacity * sizeof(SFLAdaptor *));
	adList->num_adaptors = 0;
	return adList;
}

/**
 * Frees the adaptor, using the optional freeUserData function to
 * fee memory allocated to the user data structure.
 */
static void adaptorFree(SFLAdaptor *ad, freeUserData_t freeUserData)
{
	if (ad) {
		if (ad->deviceName) {
			my_free(ad->deviceName);
		}
		if (ad->userData) {
			if (freeUserData) {
				freeUserData(ad->userData);
			} else {
				my_free(ad->userData);
			}
			ad->userData = NULL;
		}
		my_free(ad);
	}
}

void adaptorListReset(SFLAdaptorList *adList, freeUserData_t freeUserData)
{
	for (uint32_t i = 0; i < adList->num_adaptors; i++) {
		if (adList->adaptors[i]) {
			adaptorFree(adList->adaptors[i], freeUserData);
			adList->adaptors[i] = NULL;
		}
	}
	adList->num_adaptors = 0;
}

void adaptorListFree(SFLAdaptorList *adList, freeUserData_t freeUserData)
{
	adaptorListReset(adList, freeUserData);
	my_free(adList->adaptors);
	my_free(adList);
}

void adaptorListMarkAll(SFLAdaptorList *adList)
{
	for (uint32_t i = 0; i < adList->num_adaptors; i++) {
		SFLAdaptor *ad = adList->adaptors[i];
		if (ad) {
			ad->marked = TRUE;
		}
	}
}

void adaptorListFreeMarked(SFLAdaptorList *adList, freeUserData_t freeUserData)
{
	uint32_t removed = 0;
	for (uint32_t i = 0; i < adList->num_adaptors; i++) {
		SFLAdaptor *ad = adList->adaptors[i];
		if (ad && ad->marked) {
			adaptorFree(ad, freeUserData);
			adList->adaptors[i] = NULL;
			removed++;
		}
	}
	if (removed > 0) {
		uint32_t found = 0;
		// now pack the array and update the num_adaptors count
		for (uint32_t i = 0; i < adList->num_adaptors; i++) {
			SFLAdaptor *ad = adList->adaptors[i];
			if (ad) {
				adList->adaptors[found++] = ad;
			}
		}
		// cross-check
		if ((found + removed) != adList->num_adaptors) {
			myLog(LOG_ERR, "adaptorListFreeMarked: found(%u) + removed(%u) != num_adaptors(%u)",
				found,
				removed,
				adList->num_adaptors);
		}
		adList->num_adaptors = found;
	}
}
  
SFLAdaptor *adaptorListGet(SFLAdaptorList *adList, char *dev)
{
	for (uint32_t i = 0; i < adList->num_adaptors; i++) {
		SFLAdaptor *ad = adList->adaptors[i];
		if (ad && ad->deviceName && !strcmp(ad->deviceName, dev)) {
			// return the one that was already there
			return ad;
		}
	}
	return NULL;
}

SFLAdaptor *adaptorListAdd(SFLAdaptorList *adList, char *dev, u_char *macBytes, size_t userDataSize)
{
	SFLAdaptor *ad = adaptorListGet(adList, dev);
	if (ad == NULL) {
		ad = (SFLAdaptor *)my_calloc(sizeof(SFLAdaptor));
		ad->deviceName = my_strdup(dev);
		ad->userData = my_calloc(userDataSize);
		if (adList->num_adaptors == adList->capacity) {
			// grow
			adList->capacity *= 2;
			adList->adaptors = (SFLAdaptor **)my_realloc(adList->adaptors, adList->capacity * sizeof(SFLAdaptor *));
		}
		adList->adaptors[adList->num_adaptors++] = ad;
		if (macBytes) {
			memcpy(ad->macs[0].mac, macBytes, 6);
			ad->num_macs = 1;
		}
	}
	return ad;
}

#if defined(__cplusplus)
}  /* extern "C" */
#endif