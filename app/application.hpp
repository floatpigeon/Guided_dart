#ifndef DART_APPLICATION_HPP
#define DART_APPLICATION_HPP

namespace dart::app {

// Parse application options, assemble components, run and release resources.
// Notify launcher after component cleanup through DART_EXIT_MARKER when set.
int run(int argc, char* argv[]) noexcept;

} // namespace dart::app

#endif // DART_APPLICATION_HPP
