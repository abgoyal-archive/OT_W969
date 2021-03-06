
/* Created by grhoten 03/17/2004 */

/* Base class for data driven tests */

#ifndef U_TESTFW_TESTLOG
#define U_TESTFW_TESTLOG

#include "unicode/unistr.h"
#include "unicode/testtype.h"

class T_CTEST_EXPORT_API TestLog {
public:
    virtual ~TestLog();
    virtual void errln( const UnicodeString &message ) = 0;
    virtual void dataerrln( const UnicodeString &message ) = 0;
    virtual const char* getTestDataPath(UErrorCode& err) = 0;
};


#endif
