// Target of the fully-static link's --defsym for __register_frame_info and
// __deregister_frame_info (see CMakeLists.txt). Exceptions still unwind:
// libgcc finds FDEs through PT_GNU_EH_FRAME via dl_iterate_phdr.
extern "C" void* agentty_skip_frame_registration(const void*, void*) { return nullptr; }
