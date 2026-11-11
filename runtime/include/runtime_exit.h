#pragma once

// Exit immediately with `code` after the process transcript has drained. stdout and stderr go
// through a pipe pumped by transcript threads, so a bare std::_Exit can drop the last lines.
// For harness exits (race oracle) that must not run the full runtime teardown.
[[noreturn]] void RuntimeFlushAndExit(int code);
