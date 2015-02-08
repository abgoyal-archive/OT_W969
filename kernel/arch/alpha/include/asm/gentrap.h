
#ifndef _ASMAXP_GENTRAP_H
#define _ASMAXP_GENTRAP_H

#define GEN_INTOVF	-1	/* integer overflow */
#define GEN_INTDIV	-2	/* integer division by zero */
#define GEN_FLTOVF	-3	/* fp overflow */
#define GEN_FLTDIV	-4	/* fp division by zero */
#define GEN_FLTUND	-5	/* fp underflow */
#define GEN_FLTINV	-6	/* invalid fp operand */
#define GEN_FLTINE	-7	/* inexact fp operand */
#define GEN_DECOVF	-8	/* decimal overflow (for COBOL??) */
#define GEN_DECDIV	-9	/* decimal division by zero */
#define GEN_DECINV	-10	/* invalid decimal operand */
#define GEN_ROPRAND	-11	/* reserved operand */
#define GEN_ASSERTERR	-12	/* assertion error */
#define GEN_NULPTRERR	-13	/* null pointer error */
#define GEN_STKOVF	-14	/* stack overflow */
#define GEN_STRLENERR	-15	/* string length error */
#define GEN_SUBSTRERR	-16	/* substring error */
#define GEN_RANGERR	-17	/* range error */
#define GEN_SUBRNG	-18
#define GEN_SUBRNG1	-19	 
#define GEN_SUBRNG2	-20
#define GEN_SUBRNG3	-21	/* these report range errors for */
#define GEN_SUBRNG4	-22	/* subscripting (indexing) at levels 0..7 */
#define GEN_SUBRNG5	-23
#define GEN_SUBRNG6	-24
#define GEN_SUBRNG7	-25

/* the remaining codes (-26..-1023) are reserved. */

#endif /* _ASMAXP_GENTRAP_H */
