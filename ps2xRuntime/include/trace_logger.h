#ifndef TRACE_LOGGER_H
#define TRACE_LOGGER_H

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

void push_trace(const char* msg);
void dump_trace();

#ifdef __cplusplus
}
#endif

#endif // TRACE_LOGGER_H
