#include <cstdio>

// Verify all three dependency headers are on the include path.
// No runtime initialization is done here — that comes later.
#include <bgfx/bgfx.h>
#include <SDL3/SDL.h>
#include <entt/entt.hpp>

int main(int /*argc*/, char* /*argv*/[])
{
    // Smoke-test: create an EnTT registry to confirm the ECS header compiled.
    entt::registry registry;
    (void)registry;

    printf("Hello world!\n");
    return 0;
}
