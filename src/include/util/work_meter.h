#ifndef UTIL_WORK_METER_H
#define UTIL_WORK_METER_H
/* PicoBoy: no-op stub for fhoedemakers util/work_meter.h. The InfoNES core
 * sprinkles WorkMeterMark()/WorkMeterReset() calls for an on-screen profiling
 * overlay we don't use. NOTE: the MARKER_* tag values are defined by the core
 * itself (infones.cpp), so this stub must NOT declare them. */

namespace util
{
    static inline void WorkMeterMark(int)   {}
    static inline void WorkMeterReset(void) {}
}

#endif /* UTIL_WORK_METER_H */