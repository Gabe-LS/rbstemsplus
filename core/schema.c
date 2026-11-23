/* The schema's metadata, from schema.def and facts.def, as static constant data (nothing is
 * computed at start, mac-core §3.11). Each field gets its index inside its section, each column
 * its index inside its table, at compile time, so a section or a row stores only its own cells. */
#include "internal.h"

/* CORE_EQ(a, b) is 1 when a and b are the same section or table id, else 0 (the preprocessor's
 * "probe" idiom: CORE_EQ_<x>_<x> is defined below, once per section and table, and nothing else
 * is). A table without its line gets no columns; the static assertions below catch that. */
#define CORE_PROBE ~, 1
#define CORE_SECOND_(a, b, ...) b
#define CORE_SECOND(...) CORE_SECOND_(__VA_ARGS__)
#define CORE_IS_PROBE(...) CORE_SECOND(__VA_ARGS__, 0, ~)
#define CORE_EQ(a, b) CORE_IS_PROBE(CORE_EQ_##a##_##b)
#define CORE_CAT_(a, b) a##b
#define CORE_CAT(a, b) CORE_CAT_(a, b)
#define CORE_IF(c) CORE_CAT(CORE_IF_, c)
#define CORE_IF_1(...) __VA_ARGS__
#define CORE_IF_0(...)

#define CORE_EQ_S_APP_S_APP CORE_PROBE
#define CORE_EQ_S_STATUS_S_STATUS CORE_PROBE
#define CORE_EQ_S_PREPARE_S_PREPARE CORE_PROBE
#define CORE_EQ_S_SETTINGS_S_SETTINGS CORE_PROBE
#define CORE_EQ_T_STATUS_SETS_T_STATUS_SETS CORE_PROBE
#define CORE_EQ_T_PREP_OUTLINE_T_PREP_OUTLINE CORE_PROBE
#define CORE_EQ_T_PREP_TRACKS_T_PREP_TRACKS CORE_PROBE

/* each section's fields, numbered from 0 (FI_<field>), and their count (FN_<section>) */
enum {
#define FIELD(id, sec, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(sec, S_APP))(FI_##id,))
#include "schema.def"
    FN_S_APP
};
enum {
#define FIELD(id, sec, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(sec, S_STATUS))(FI_##id,))
#include "schema.def"
    FN_S_STATUS
};
enum {
#define FIELD(id, sec, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(sec, S_PREPARE))(FI_##id,))
#include "schema.def"
    FN_S_PREPARE
};
enum {
#define FIELD(id, sec, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(sec, S_SETTINGS))(FI_##id,))
#include "schema.def"
    FN_S_SETTINGS
};

/* each table's columns, numbered from 0 (CI_<column>), and their count (CN_<table>) */
enum {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(table, T_STATUS_SETS))(CI_##id,))
#include "schema.def"
    CN_T_STATUS_SETS
};
enum {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(table, T_PREP_OUTLINE))(CI_##id,))
#include "schema.def"
    CN_T_PREP_OUTLINE
};
enum {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(table, T_PREP_TRACKS))(CI_##id,))
#include "schema.def"
    CN_T_PREP_TRACKS
};
#ifdef CORE_WIN_POLICY
#define CORE_EQ_T_WIN_RB_VERSIONS_T_WIN_RB_VERSIONS CORE_PROBE
enum {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os(CORE_IF(CORE_EQ(table, T_WIN_RB_VERSIONS))(CI_##id,))
#include "schema.def"
    CN_T_WIN_RB_VERSIONS
};
#endif

/* every field is in one section's numbering, every column in one table's */
_Static_assert(0
#define SECTION(id, doc) + FN_##id
#include "schema.def"
    == CORE_FIELD_COUNT, "a section without its CORE_EQ line or numbering block in schema.c");
_Static_assert(0
#define TABLE(id, section, os, doc) CORE_KEEP_##os(+ CN_##id)
#include "schema.def"
    == CORE_COLUMN_COUNT, "a table without its CORE_EQ line or numbering block in schema.c");
_Static_assert(CORE_SECTION_COUNT <= 32, "core_delta_sections is a 32-bit mask");

const section_meta core_section_meta[CORE_SECTION_COUNT] = {
#define SECTION(id, doc) [id] = {#id, FN_##id},
#include "schema.def"
};

const table_meta core_table_meta[CORE_TABLE_COUNT > 0 ? CORE_TABLE_COUNT : 1] = {
#define TABLE(id, section, os, doc) CORE_KEEP_##os([id] = {#id, section, CN_##id},)
#include "schema.def"
};

const field_meta core_field_meta[CORE_FIELD_COUNT > 0 ? CORE_FIELD_COUNT : 1] = {
#define FIELD(id, sec, kind, os, doc) CORE_KEEP_##os([id] = {#id, sec, CORE_KIND_##kind, FI_##id},)
#include "schema.def"
};

const column_meta core_column_meta[CORE_COLUMN_COUNT > 0 ? CORE_COLUMN_COUNT : 1] = {
#define COLUMN(id, table, kind, os, doc) CORE_KEEP_##os([id] = {#id, table, CORE_KIND_##kind, CI_##id},)
#include "schema.def"
};

const fact_meta core_fact_meta[CORE_FACT_COUNT > 0 ? CORE_FACT_COUNT : 1] = {
#define FACT(id, kind, os, doc) CORE_KEEP_##os([id] = {#id, CORE_KIND_##kind},)
#include "facts.def"
};
