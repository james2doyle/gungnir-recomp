namespace psprecomp {
class Runtime;
void register_generated_functions(Runtime &) {}
}

namespace gungnir::session {

bool generated_corpus_present() noexcept {
    return false;
}

} // namespace gungnir::session
