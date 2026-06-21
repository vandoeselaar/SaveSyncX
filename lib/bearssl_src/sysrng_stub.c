/*
 * sysrng_stub.c - vervangt sysrng.c voor nxdk/Xbox
 *
 * br_prng_seeder_system geeft NULL terug zodat BearSSL weet dat er
 * geen OS-RNG beschikbaar is. De engine valt dan terug op extern
 * geïnjecteerde entropy (br_ssl_engine_inject_entropy in main.c).
 */

#include "inner.h"

br_prng_seeder
br_prng_seeder_system(const char **name)
{
    if (name != NULL) {
        *name = "none";
    }
    return NULL;
}
