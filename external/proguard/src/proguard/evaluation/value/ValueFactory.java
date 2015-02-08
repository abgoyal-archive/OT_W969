
package proguard.evaluation.value;

import proguard.classfile.*;
import proguard.classfile.util.ClassUtil;

public class ValueFactory
{
    // Shared copies of Value objects, to avoid creating a lot of objects.
    static final IntegerValue INTEGER_VALUE = new UnknownIntegerValue();
    static final LongValue    LONG_VALUE    = new UnknownLongValue();
    static final FloatValue   FLOAT_VALUE   = new UnknownFloatValue();
    static final DoubleValue  DOUBLE_VALUE  = new UnknownDoubleValue();

    static final ReferenceValue REFERENCE_VALUE_NULL                        = new ReferenceValue(null, null, true);
    static final ReferenceValue REFERENCE_VALUE_JAVA_LANG_OBJECT_MAYBE_NULL = new ReferenceValue(ClassConstants.INTERNAL_NAME_JAVA_LANG_OBJECT, null, true);
    static final ReferenceValue REFERENCE_VALUE_JAVA_LANG_OBJECT_NOT_NULL   = new ReferenceValue(ClassConstants.INTERNAL_NAME_JAVA_LANG_OBJECT, null, false);


    /**
     * Creates a new Value of the given type.
     * The type must be a fully specified internal type for primitives, classes,
     * or arrays.
     */
    public Value createValue(String type, Clazz referencedClass, boolean mayBeNull)
    {
        switch (type.charAt(0))
        {
            case ClassConstants.INTERNAL_TYPE_VOID:    return null;
            case ClassConstants.INTERNAL_TYPE_BOOLEAN:
            case ClassConstants.INTERNAL_TYPE_BYTE:
            case ClassConstants.INTERNAL_TYPE_CHAR:
            case ClassConstants.INTERNAL_TYPE_SHORT:
            case ClassConstants.INTERNAL_TYPE_INT:     return createIntegerValue();
            case ClassConstants.INTERNAL_TYPE_LONG:    return createLongValue();
            case ClassConstants.INTERNAL_TYPE_FLOAT:   return createFloatValue();
            case ClassConstants.INTERNAL_TYPE_DOUBLE:  return createDoubleValue();
            default:                                   return createReferenceValue(ClassUtil.isInternalArrayType(type) ?
                                                                                       type :
                                                                                       ClassUtil.internalClassNameFromClassType(type),
                                                                                   referencedClass,
                                                                                   mayBeNull);
        }
    }

    /**
     * Creates a new IntegerValue with an undefined value.
     */
    public IntegerValue createIntegerValue()
    {
        return INTEGER_VALUE;
    }

    /**
     * Creates a new IntegerValue with a given particular value.
     */
    public IntegerValue createIntegerValue(int value)
    {
        return createIntegerValue();
    }


    /**
     * Creates a new LongValue with an undefined value.
     */
    public LongValue createLongValue()
    {
        return LONG_VALUE;
    }

    /**
     * Creates a new LongValue with a given particular value.
     */
    public LongValue createLongValue(long value)
    {
        return createLongValue();
    }


    /**
     * Creates a new FloatValue with an undefined value.
     */
    public FloatValue createFloatValue()
    {
        return FLOAT_VALUE;
    }

    /**
     * Creates a new FloatValue with a given particular value.
     */
    public FloatValue createFloatValue(float value)
    {
        return createFloatValue();
    }


    /**
     * Creates a new DoubleValue with an undefined value.
     */
    public DoubleValue createDoubleValue()
    {
        return DOUBLE_VALUE;
    }

    /**
     * Creates a new DoubleValue with a given particular value.
     */
    public DoubleValue createDoubleValue(double value)
    {
        return createDoubleValue();
    }


    /**
     * Creates a new ReferenceValue that represents <code>null</code>.
     */
    public ReferenceValue createReferenceValueNull()
    {
        return REFERENCE_VALUE_NULL;
    }


    /**
     * Creates a new ReferenceValue of the given type. The type must be an
     * internal class name or an array type. If the type is <code>null</code>,
     * the ReferenceValue represents <code>null</code>.
     */
    public ReferenceValue createReferenceValue(String  type,
                                               Clazz   referencedClass,
                                               boolean mayBeNull)
    {
        return type == null                                                ? REFERENCE_VALUE_NULL                                 :
               !type.equals(ClassConstants.INTERNAL_NAME_JAVA_LANG_OBJECT) ? new ReferenceValue(type, referencedClass, mayBeNull) :
               mayBeNull                                                   ? REFERENCE_VALUE_JAVA_LANG_OBJECT_MAYBE_NULL          :
                                                                             REFERENCE_VALUE_JAVA_LANG_OBJECT_NOT_NULL;
    }


    /**
     * Creates a new ReferenceValue for arrays of the given type and length.
     * The type must be a fully specified internal type for primitives, classes,
     * or arrays.
     */
    public ReferenceValue createArrayReferenceValue(String       type,
                                                    Clazz        referencedClass,
                                                    IntegerValue arrayLength)
    {
        return createArrayReferenceValue(type,
                                         referencedClass,
                                         arrayLength,
                                         createValue(type, referencedClass, false));
    }


    /**
     * Creates a new ReferenceValue for arrays of the given type and length,
     * containing the given element. The type must be a fully specified internal
     * type for primitives, classes, or arrays.
     */
    public ReferenceValue createArrayReferenceValue(String       type,
                                                    Clazz        referencedClass,
                                                    IntegerValue arrayLength,
                                                    Value        elementValue)
    {
        return createReferenceValue(ClassConstants.INTERNAL_TYPE_ARRAY + type,
                                    referencedClass,
                                    false);
    }
}