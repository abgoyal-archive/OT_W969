
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <dwarf.h>
#include "libdwP.h"


int
dwarf_arrayorder (die)
     Dwarf_Die *die;
{
  Dwarf_Attribute attr_mem;
  Dwarf_Word value;

  return dwarf_formudata (dwarf_attr (die, DW_AT_ordering, &attr_mem),
			  &value) == 0 ? (int) value : -1;
}
