#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <veach/core/log.hpp>

int main(int argc, char** argv) {
    // Engine instances re-configure logging; this keeps direct library tests quiet too.
    veach::log::configure({.console = false});
    doctest::Context context(argc, argv);
    return context.run();
}
