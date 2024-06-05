#ifndef SLATE_UTILS_H
#define SLATE_UTILS_H

#include <stdint.h>

#include "qemu/osdep.h"

#include "qapi/visitor.h"
#include "qom/object.h"

#ifndef OFFSET_OF
#define OFFSET_OF(struct, member) ((uintptr_t) __builtin_offsetof(struct, member))
#endif

void get_uint32(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp);
void set_uint32(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp);

#endif