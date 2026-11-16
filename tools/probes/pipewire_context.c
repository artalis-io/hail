#include <pipewire/pipewire.h>
int main(void) {
    pw_init(0, 0);
    struct pw_main_loop *loop = pw_main_loop_new(0);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), 0, 0);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();
    return 0;
}
