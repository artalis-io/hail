#include <hail.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout<ha_stream_status>::value, "C ABI record");
static_assert(sizeof(ha_result) == 4, "result width");
static_assert(sizeof(ha_format) == 32, "format layout");
static_assert(offsetof(ha_block_info, first_frame) == 8, "block prefix");
static_assert(offsetof(ha_stream_status, playback_generation) == 16, "status prefix");

int main()
{
    struct Extended { ha_version_info v; std::uint64_t canary; } e = {};
    e.v = HA_INIT(ha_version_info);
    e.v.struct_size = sizeof(e);
    e.canary = UINT64_C(0x123456789abcdef0);
    if (ha_get_version(&e.v) != HA_OK || e.canary != UINT64_C(0x123456789abcdef0)) return 1;
    if (e.v.struct_size != sizeof(e) || e.v.major != 0 || e.v.minor != 1) return 2;
    e.v.struct_size = 8;
    if (ha_get_version(&e.v) != HA_INVALID_ARGUMENT) return 3;
    e.v = HA_INIT(ha_version_info); e.v.struct_version = 2;
    return ha_get_version(&e.v) == HA_INVALID_ARGUMENT ? 0 : 4;
}
