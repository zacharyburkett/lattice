#ifndef LATTICE_TYPES_H
#define LATTICE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t lt_entity_t;
typedef uint32_t lt_component_id_t;

enum {
    LT_ENTITY_NULL = 0u,
    LT_COMPONENT_INVALID = 0u
};

typedef enum lt_status_e {
    LT_STATUS_OK = 0,
    LT_STATUS_INVALID_ARGUMENT = 1,
    LT_STATUS_NOT_FOUND = 2,
    LT_STATUS_ALREADY_EXISTS = 3,
    LT_STATUS_CAPACITY_REACHED = 4,
    LT_STATUS_ALLOCATION_FAILED = 5,
    LT_STATUS_STALE_ENTITY = 6,
    LT_STATUS_CONFLICT = 7,
    LT_STATUS_NOT_IMPLEMENTED = 8
} lt_status_t;

const char* lt_status_string(lt_status_t status);

#ifdef __cplusplus
}
#endif

#endif
