#ifndef RBT_DATA_H
#define RBT_DATA_H

#include "unicode/utypes.h"
#include "unicode/uclean.h"

#if !UCONFIG_NO_TRANSLITERATION

#include "unicode/uobject.h"
#include "rbt_set.h"
#include "hash.h"

U_NAMESPACE_BEGIN

class UnicodeFunctor;
class UnicodeMatcher;
class UnicodeReplacer;

class TransliterationRuleData : public UMemory {

public:

    // PUBLIC DATA MEMBERS

    /**
     * Rule table.  May be empty.
     */
    TransliterationRuleSet ruleSet;

    /**
     * Map variable name (String) to variable (UnicodeString).  A variable name
     * corresponds to zero or more characters, stored in a UnicodeString in
     * this hash.  One or more of these chars may also correspond to a
     * UnicodeMatcher, in which case the character in the UnicodeString in this hash is
     * a stand-in: it is an index for a secondary lookup in
     * data.variables.  The stand-in also represents the UnicodeMatcher in
     * the stored rules.
     */
    Hashtable variableNames;

    /**
     * Map category variable (UChar) to set (UnicodeFunctor).
     * Variables that correspond to a set of characters are mapped
     * from variable name to a stand-in character in data.variableNames.
     * The stand-in then serves as a key in this hash to lookup the
     * actual UnicodeFunctor object.  In addition, the stand-in is
     * stored in the rule text to represent the set of characters.
     * variables[i] represents character (variablesBase + i).
     */
    UnicodeFunctor** variables;

    /**
     * Flag that indicates whether the variables are owned (if a single
     * call to Transliterator::createFromRules() produces a CompoundTransliterator
     * with more than one RuleBasedTransliterator as children, they all share
     * the same variables list, so only the first one is considered to own
     * the variables)
     */
    UBool variablesAreOwned;

    /**
     * The character that represents variables[0].  Characters
     * variablesBase through variablesBase +
     * variablesLength - 1 represent UnicodeFunctor objects.
     */
    UChar variablesBase;

    /**
     * The length of variables.
     */
    int32_t variablesLength;

public:

    /**
     * Constructor
     * @param status Output param set to success/failure code on exit.
     */
    TransliterationRuleData(UErrorCode& status);

    /**
     * Copy Constructor
     */
    TransliterationRuleData(const TransliterationRuleData&);

    /**
     * destructor
     */
    ~TransliterationRuleData();

    /**
     * Given a stand-in character, return the UnicodeFunctor that it
     * represents, or NULL if it doesn't represent anything.
     * @param standIn    the given stand-in character.
     * @return           the UnicodeFunctor that 'standIn' represents
     */
    UnicodeFunctor* lookup(UChar32 standIn) const;

    /**
     * Given a stand-in character, return the UnicodeMatcher that it
     * represents, or NULL if it doesn't represent anything or if it
     * represents something that is not a matcher.
     * @param standIn    the given stand-in character.
     * @return           return the UnicodeMatcher that 'standIn' represents
     */
    UnicodeMatcher* lookupMatcher(UChar32 standIn) const;

    /**
     * Given a stand-in character, return the UnicodeReplacer that it
     * represents, or NULL if it doesn't represent anything or if it
     * represents something that is not a replacer.
     * @param standIn    the given stand-in character.
     * @return           return the UnicodeReplacer that 'standIn' represents
     */
    UnicodeReplacer* lookupReplacer(UChar32 standIn) const;


private:
    TransliterationRuleData &operator=(const TransliterationRuleData &other); // forbid copying of this class
};

U_NAMESPACE_END

#endif /* #if !UCONFIG_NO_TRANSLITERATION */

#endif
