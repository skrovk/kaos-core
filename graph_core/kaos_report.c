#include "kaos_report.h"
#include "kaos_cbor.h"

static bool node_add(uint8_t kind) { return kind == 1 || kind == 2; }
static bool removal(uint8_t kind) { return kind >= 4 && kind <= 6; }

static unsigned fact_count(unsigned facts)
{
    unsigned count = 0;
    while (facts != 0) { count += facts & 1u; facts >>= 1; }
    return count;
}

uint8_t core_report_revision_bound(uint8_t count)
{
    return count > 0 && count <= CORE_REPORT_MAX_OBJECTS ? (uint8_t)(8 * count + 6) : 0;
}

static bool object_valid(uint8_t f)
{
    return (!(f & CORE_OBJECT_ACTIVE) || (f & CORE_OBJECT_READY)) &&
        (!(f & CORE_OBJECT_STOPPED) || (f & CORE_OBJECT_CLEANUP)) &&
        (!(f & CORE_OBJECT_CLEANED) || (f & CORE_OBJECT_STOPPED));
}

static bool valid(const core_report *r)
{
    if (r->kind < 1 || r->kind > 6 || r->object_count == 0 ||
        r->object_count > CORE_REPORT_MAX_OBJECTS ||
        r->revision > core_report_revision_bound(r->object_count) ||
        (r->history & ~63u)) return false;
    if (((r->kind == 1 || r->kind == 4) && r->object_count != 1) ||
        ((r->kind == 3 || r->kind == 6) && r->object_count != 2) ||
        ((r->kind == 2 || r->kind == 5) && !(r->object_count & 1))) return false;
    for (uint8_t i = 0; i < r->object_count; ++i)
        if (!object_valid(r->objects[i])) return false;
    const unsigned h = r->history;
    unsigned changes = fact_count(h) - (removal(r->kind) && (h & CORE_HISTORY_CLEANUP) ? 1u : 0u);
    for (uint8_t i = 0; i < r->object_count; ++i) changes += fact_count(r->objects[i]);
    if (r->revision > changes || ((r->revision == 0) != (changes == 0))) return false;
    if (removal(r->kind) && ((h & (CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
            CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS)) || !(h & CORE_HISTORY_CLEANUP)))
        return false;
    if ((h & CORE_HISTORY_DISPATCH) && (!(h & CORE_HISTORY_COMMIT) || !node_add(r->kind)))
        return false;
    if ((h & CORE_HISTORY_STARTED) && !(h & CORE_HISTORY_DISPATCH)) return false;
    if ((h & CORE_HISTORY_CUTOFF) && !(h & CORE_HISTORY_CLEANUP)) return false;
    if (h & CORE_HISTORY_SUCCESS) {
        if (node_add(r->kind)) {
            if (!(h & CORE_HISTORY_STARTED)) return false;
        } else if (r->kind == 3) {
            if (!(h & CORE_HISTORY_COMMIT) ||
                !(r->objects[0] & CORE_OBJECT_ACTIVE) ||
                !(r->objects[1] & CORE_OBJECT_ACTIVE)) return false;
        } else return false;
    }
    return true;
}

bool core_report_init(core_report *r, uint8_t kind, core_op_id id,
                      uint64_t coordinator, uint8_t count)
{
    core_report initial = {.id = id, .coordinator = coordinator,
        .kind = kind, .object_count = count,
        .history = removal(kind) ? CORE_HISTORY_CLEANUP : 0};
    if (!valid(&initial)) return false;
    *r = initial;
    return true;
}

uint16_t core_report_obligations(const core_report *r)
{
    uint16_t missing = 0;
    for (uint8_t i = 0; i < r->object_count; ++i) {
        const unsigned f = r->objects[i];
        if (((r->history & CORE_HISTORY_CLEANUP) || (f & CORE_OBJECT_CLEANUP)) &&
            !(f & CORE_OBJECT_CLEANED)) missing |= (uint16_t)(1u << i);
    }
    return missing;
}

core_outcome core_report_outcome(const core_report *r)
{
    if (r->history & CORE_HISTORY_SUCCESS) return CORE_OUTCOME_SUCCEEDED;
    if (r->history & CORE_HISTORY_CLEANUP) {
        if (core_report_obligations(r) == 0)
            return removal(r->kind) ? CORE_OUTCOME_SUCCEEDED : CORE_OUTCOME_FAILED_CLEAN;
        if (r->history & CORE_HISTORY_CUTOFF) return CORE_OUTCOME_UNRESOLVED;
    }
    return CORE_OUTCOME_PENDING;
}

core_report_result core_report_observe(core_report *r, uint8_t index, uint8_t facts)
{
    if (!valid(r) || index >= r->object_count || !object_valid(facts))
        return CORE_REPORT_INVALID;
    const uint8_t old = r->objects[index];
    if (facts == old) return CORE_REPORT_REPEAT;
    if ((facts & old) == facts) return CORE_REPORT_OLD;
    if ((facts & old) != old) return CORE_REPORT_CONFLICT;
    if (r->revision == core_report_revision_bound(r->object_count)) return CORE_REPORT_EXHAUSTED;
    r->objects[index] = facts;
    ++r->revision;
    return CORE_REPORT_CHANGED;
}

core_report_result core_report_record(core_report *r, uint8_t history)
{
    if (!valid(r) || (history & ~63u)) return CORE_REPORT_INVALID;
    if ((r->history | history) == r->history) return CORE_REPORT_REPEAT;
    const unsigned constructive = CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS;
    if ((r->history & CORE_HISTORY_CLEANUP) && (history & ~r->history & constructive))
        return CORE_REPORT_INVALID;
    if ((history & CORE_HISTORY_CUTOFF) && core_report_obligations(r) == 0)
        return CORE_REPORT_INVALID;
    if (r->revision == core_report_revision_bound(r->object_count)) return CORE_REPORT_EXHAUSTED;
    core_report next = *r;
    next.history |= history;
    ++next.revision;
    if (!valid(&next)) return CORE_REPORT_INVALID;
    *r = next;
    return CORE_REPORT_CHANGED;
}

bool core_report_encode(const core_report *r, uint8_t *data, size_t size, size_t *written)
{
    if (!valid(r)) return false;
    core_cbor_writer w = {.data = data, .size = size};
    core_cbor_write_array(&w, 8);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, CORE_REPORT_KIND);
    core_cbor_write_uint(&w, r->id.value);
    core_cbor_write_uint(&w, r->coordinator);
    core_cbor_write_uint(&w, r->revision);
    core_cbor_write_uint(&w, core_report_outcome(r));
    core_cbor_write_uint(&w, r->history);
    core_cbor_write_array(&w, r->object_count);
    for (uint8_t i = 0; i < r->object_count; ++i) {
        core_cbor_write_uint(&w, r->objects[i]);
    }
    if (w.failed) return false;
    *written = w.offset;
    return true;
}

bool core_report_decode(const uint8_t *data, size_t size, uint8_t kind, core_report *out)
{
    if (data == NULL || size == 0 || size > CORE_REPORT_BYTES) return false;
    core_cbor_reader rd = {.data = data, .size = size};
    uint64_t count, version, tag, revision, outcome, history, objects;
    core_report r = {.kind = kind};
    if (!core_cbor_array(&rd, &count) || count != 8 ||
        !core_cbor_uint(&rd, &version) || version != CORE_SCHEMA_VERSION ||
        !core_cbor_uint(&rd, &tag) || tag != CORE_REPORT_KIND ||
        !core_cbor_uint(&rd, &r.id.value) || !core_cbor_uint(&rd, &r.coordinator) ||
        !core_cbor_uint(&rd, &revision) || revision > UINT8_MAX ||
        !core_cbor_uint(&rd, &outcome) || outcome > CORE_OUTCOME_UNRESOLVED ||
        !core_cbor_uint(&rd, &history) || history > UINT8_MAX ||
        !core_cbor_array(&rd, &objects) || objects > CORE_REPORT_MAX_OBJECTS) return false;
    r.revision = (uint8_t)revision;
    r.history = (uint8_t)history;
    r.object_count = (uint8_t)objects;
    for (uint8_t i = 0; i < r.object_count; ++i) {
        uint64_t facts;
        if (!core_cbor_uint(&rd, &facts) || facts > UINT8_MAX) return false;
        r.objects[i] = (uint8_t)facts;
    }
    if (rd.offset != size || !valid(&r) || outcome != core_report_outcome(&r)) return false;
    *out = r;
    return true;
}

core_report_result core_report_accept(core_report *r, const core_report *next)
{
    if (!valid(r) || !valid(next) || r->id.value != next->id.value ||
        r->coordinator != next->coordinator || r->kind != next->kind ||
        r->object_count != next->object_count) return CORE_REPORT_INVALID;
    if (next->revision < r->revision) return CORE_REPORT_OLD;
    bool equal = r->history == next->history;
    for (uint8_t i = 0; i < r->object_count; ++i) {
        equal = equal && r->objects[i] == next->objects[i];
    }
    if (next->revision == r->revision) return equal ? CORE_REPORT_REPEAT : CORE_REPORT_CONFLICT;
    if (equal || (next->history & r->history) != r->history) return CORE_REPORT_INVALID;
    const unsigned constructive = CORE_HISTORY_COMMIT | CORE_HISTORY_DISPATCH |
        CORE_HISTORY_STARTED | CORE_HISTORY_SUCCESS;
    if ((r->history & CORE_HISTORY_CLEANUP) && (next->history & ~r->history & constructive))
        return CORE_REPORT_INVALID;
    for (uint8_t i = 0; i < r->object_count; ++i) {
        if ((next->objects[i] & r->objects[i]) != r->objects[i]) return CORE_REPORT_INVALID;
    }
    *r = *next;
    return CORE_REPORT_CHANGED;
}

bool core_owner_report_encode(const core_owner_report *r, uint8_t *data,
                              size_t size, size_t *written)
{
    if (r->object >= CORE_REPORT_MAX_OBJECTS || !object_valid(r->facts)) return false;
    core_cbor_writer w = {.data = data, .size = size};
    core_cbor_write_array(&w, 6);
    core_cbor_write_uint(&w, CORE_SCHEMA_VERSION);
    core_cbor_write_uint(&w, CORE_OWNER_REPORT_KIND);
    core_cbor_write_uint(&w, r->operation.value);
    core_cbor_write_uint(&w, r->owner);
    core_cbor_write_uint(&w, r->object);
    core_cbor_write_uint(&w, r->facts);
    if (w.failed) return false;
    *written = w.offset;
    return true;
}

bool core_owner_report_decode(const uint8_t *data, size_t size, core_owner_report *out)
{
    if (data == NULL || size == 0 || size > CORE_REPORT_BYTES) return false;
    core_cbor_reader rd = {.data = data, .size = size};
    core_owner_report r;
    uint64_t count, version, kind, index, facts;
    if (!core_cbor_array(&rd, &count) || count != 6 ||
        !core_cbor_uint(&rd, &version) || version != CORE_SCHEMA_VERSION ||
        !core_cbor_uint(&rd, &kind) || kind != CORE_OWNER_REPORT_KIND ||
        !core_cbor_uint(&rd, &r.operation.value) || !core_cbor_uint(&rd, &r.owner) ||
        !core_cbor_uint(&rd, &index) || index >= CORE_REPORT_MAX_OBJECTS ||
        !core_cbor_uint(&rd, &facts) || facts > UINT8_MAX || rd.offset != size) return false;
    r.object = (uint8_t)index;
    r.facts = (uint8_t)facts;
    if (!object_valid(r.facts)) return false;
    *out = r;
    return true;
}
