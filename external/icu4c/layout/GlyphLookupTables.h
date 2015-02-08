
#ifndef __GLYPHLOOKUPTABLES_H
#define __GLYPHLOOKUPTABLES_H


#include "LETypes.h"
#include "OpenTypeTables.h"

U_NAMESPACE_BEGIN

struct GlyphLookupTableHeader
{
    fixed32 version;
    Offset  scriptListOffset;
    Offset  featureListOffset;
    Offset  lookupListOffset;

    le_bool coversScript(LETag scriptTag) const;
    le_bool coversScriptAndLanguage(LETag scriptTag, LETag languageTag, le_bool exactMatch = FALSE) const;
};

U_NAMESPACE_END

#endif

