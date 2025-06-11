#include "Crypto.hpp"
#include <sodium.h>
#include <stdexcept>

int init_crypto() {
    return sodium_init();
}