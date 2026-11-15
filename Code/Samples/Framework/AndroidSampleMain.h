// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Android only, force-included into an RHI sample's sources (util_add_sample_app): the app's
// activity (SDL's Java glue) loads the sample as a shared library and calls its SDL_main, found by
// name, so the sample's `int main(int argc, char** argv)` is that function, with C linkage.
#pragma once

extern "C" int SDL_main(int argc, char* argv[]);
#define main SDL_main
