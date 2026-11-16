// shop: loads its configuration from the environment against contract.json,
// prints one line and exits 0, or lists every problem and exits 1. There is
// no real server, database or queue.
#include <iostream>

#include "shop.hpp"

int main() {
    // load_process_env() also writes the problems to the termination log
    // and warns about variables that look like typos of declared ones.
    return shop::boot([] { return shop::contract().load_process_env(); }, std::cout, std::cerr);
}
