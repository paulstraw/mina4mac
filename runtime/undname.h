// MSVC type name undecoration (undname.c).
#pragma once

// Undecorate an RTTI type name (".?AV...", the form stored in type_info) the way type_info::name does.
// Returns a malloc'd string, or NULL if the name uses something undname.c doesn't support.
char *undname_type(const char *decorated);
