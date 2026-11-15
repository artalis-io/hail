#include <hail.h>
int main(void)
{
    ha_version_info v = HA_INIT(ha_version_info);
    if (ha_get_version(&v) != HA_OK || v.major != 0 || v.minor != 1) return 1;
    return ha_context_destroy((ha_context **)0) == HA_INVALID_ARGUMENT ? 0 : 2;
}
