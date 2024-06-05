#include "slate_utils.h"

void get_uint32(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
  uintptr_t ptr = (uintptr_t)obj + (uintptr_t)opaque;
  uint32_t* vptr = (uint32_t*)ptr;

  visit_type_uint32(v, name, vptr, errp);
}

void set_uint32(Object* obj, Visitor* v, const char* name, void* opaque, Error** errp) {
  uintptr_t ptr = (uintptr_t)obj + (uintptr_t)opaque;
  uint32_t* vptr = (uint32_t*)ptr;

  if (!visit_type_uint32(v, name, vptr, errp)) {
    printf("Update Failed.\n");
    return;
  }
}