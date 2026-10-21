#ifndef XV6_VMINFO_H
#define XV6_VMINFO_H

#include "types.h"

struct vminfo {
  uint physical_page;
  uint flags;
};

#endif
