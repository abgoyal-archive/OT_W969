
#include "unicode/utypes.h"
#include "unicode/uchar.h"
#include "unicode/unistr.h"
#include "unicode/ures.h"
#include "unicode/ustring.h"
#include "unicode/uloc.h"
#include "unicode/schriter.h"
#include "unicode/numsys.h"

#include "uresimp.h"

#if !UCONFIG_NO_FORMATTING

U_NAMESPACE_BEGIN

// Useful constants

#define DEFAULT_DIGITS UNICODE_STRING_SIMPLE("0123456789");
static const char gNumberingSystems[] = "numberingSystems";
static const char gDefaultNumberingSystem[] = "defaultNumberingSystem";
static const char gDesc[] = "desc";
static const char gRadix[] = "radix";
static const char gAlgorithmic[] = "algorithmic";


UOBJECT_DEFINE_RTTI_IMPLEMENTATION(NumberingSystem)

    /**
     * Default Constructor.
     *
     * @draft ICU 4.2
     */

NumberingSystem::NumberingSystem() {
     radix = 10;
     algorithmic = FALSE;
     UnicodeString defaultDigits = DEFAULT_DIGITS;
     desc.setTo(defaultDigits);
}

    /**
     * Copy constructor.
     * @draft ICU 4.2
     */

NumberingSystem::NumberingSystem(const NumberingSystem& other) 
:  UObject(other) {
    *this=other;
}

NumberingSystem* U_EXPORT2
NumberingSystem::createInstance(int32_t radix_in, UBool isAlgorithmic_in, const UnicodeString & desc_in, UErrorCode &status) {

    if ( radix_in < 2 ) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return NULL;
    }

    if ( !isAlgorithmic_in ) {
       if ( desc_in.countChar32() != radix_in || !isValidDigitString(desc_in)) {
           status = U_ILLEGAL_ARGUMENT_ERROR;
           return NULL;
       }
    }

    NumberingSystem *ns = new NumberingSystem();

    ns->setRadix(radix_in);
    ns->setDesc(desc_in);
    ns->setAlgorithmic(isAlgorithmic_in);
    return ns;
    
}


NumberingSystem* U_EXPORT2
NumberingSystem::createInstance(const Locale & inLocale, UErrorCode& status) {

    char buffer[ULOC_KEYWORDS_CAPACITY];
    int32_t count = inLocale.getKeywordValue("numbers",buffer, sizeof(buffer),status);
    if ( count > 0 ) { // @numbers keyword was specified in the locale
        buffer[count] = '\0'; // Make sure it is null terminated.
        return NumberingSystem::createInstanceByName(buffer,status);
    } else { // Find the default numbering system for this locale.
        UResourceBundle *resource = ures_open(NULL,inLocale.getName(),&status);
        if (U_FAILURE(status)) {
            status = U_USING_FALLBACK_WARNING;
            NumberingSystem *ns = new NumberingSystem();
            ures_close(resource);
            return ns;
        } 
        const UChar *defaultNSName = ures_getStringByKeyWithFallback(resource,gDefaultNumberingSystem,&count,&status);
        if ( count > 0 && count < ULOC_KEYWORDS_CAPACITY ) { // Default numbering system found
           u_UCharsToChars(defaultNSName,buffer,count); 
           buffer[count] = '\0'; // Make sure it is null terminated.
           ures_close(resource);
           return NumberingSystem::createInstanceByName(buffer,status);
        } else {
            status = U_USING_FALLBACK_WARNING;
            NumberingSystem *ns = new NumberingSystem();
            ures_close(resource);
            return ns;
        }
        
    }
}

NumberingSystem* U_EXPORT2
NumberingSystem::createInstance(UErrorCode& status) {
    return NumberingSystem::createInstance(Locale::getDefault(), status);
}

NumberingSystem* U_EXPORT2
NumberingSystem::createInstanceByName(const char *name, UErrorCode& status) {
    
     UResourceBundle *numberingSystemsInfo = NULL;
     UResourceBundle *nsTop, *nsCurrent;
     const UChar* description = NULL;
     int32_t radix = 10;
     int32_t algorithmic = 0;
     int32_t len;

     numberingSystemsInfo = ures_openDirect(NULL,gNumberingSystems, &status);
     nsCurrent = ures_getByKey(numberingSystemsInfo,gNumberingSystems,NULL,&status);
     nsTop = ures_getByKey(nsCurrent,name,NULL,&status);
     description = ures_getStringByKey(nsTop,gDesc,&len,&status);

	 ures_getByKey(nsTop,gRadix,nsCurrent,&status);
     radix = ures_getInt(nsCurrent,&status);

     ures_getByKey(nsTop,gAlgorithmic,nsCurrent,&status);
     algorithmic = ures_getInt(nsCurrent,&status);

     UBool isAlgorithmic = ( algorithmic == 1 );
     UnicodeString nsd;
     nsd.setTo(description);

	 ures_close(nsCurrent);
	 ures_close(nsTop);
     ures_close(numberingSystemsInfo);

     if (U_FAILURE(status)) {
         status = U_UNSUPPORTED_ERROR;
         return NULL;
     }

     return NumberingSystem::createInstance(radix,isAlgorithmic,nsd,status);

}

    /**
     * Destructor.
     * @draft ICU 4.2
     */
NumberingSystem::~NumberingSystem() {
}

int32_t NumberingSystem::getRadix() {
    return radix;
}

UnicodeString NumberingSystem::getDescription() {
    return desc;
}

void NumberingSystem::setRadix(int32_t r) {
    radix = r;
}

void NumberingSystem::setAlgorithmic(UBool c) {
    algorithmic = c;
}

void NumberingSystem::setDesc(UnicodeString d) {
    desc.setTo(d);
}

UBool NumberingSystem::isAlgorithmic() const {
    return ( algorithmic );
}


UBool NumberingSystem::isValidDigitString(const UnicodeString& str) {

    StringCharacterIterator it(str);
    UChar32 c;
    UChar32 prev = 0;
    int32_t i = 0;

    for ( it.setToStart(); it.hasNext(); ) {
       c = it.next32PostInc();
       if ( u_charDigitValue(c) != i ) {  // Digits outside the Unicode decimal digit class are not currently supported
           return FALSE;
       }
       if ( prev != 0 && c != prev + 1 ) { // Non-contiguous digits are not currently supported
          return FALSE;
       }
       if ( c > 0xFFFF ) { // Digits outside the BMP are not currently supported
          return FALSE;
       }
       i++;
       prev = c;
    }
    return TRUE;   
}
U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_FORMATTING */

//eof
