# System dependencies. The core is dependency-free apart from the OS:
#  - Threads (std::thread)
#  - dl (shared-library loading for the PluginManager; CMAKE_DL_LIBS covers it)
find_package(Threads REQUIRED)
