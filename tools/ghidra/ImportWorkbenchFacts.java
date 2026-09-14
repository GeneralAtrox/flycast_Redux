//Annotate the program with runtime facts recorded by the Flycast research workbench:
//create functions at observed call targets, add computed call/jump references for
//indirect branches the static analysis could not resolve, and summarise memory writers
//and TA (PowerVR tile accelerator) submitters as comments and bookmarks.
//Input: JSON produced by tools/workbench/flycast_workbench/queries.py::ghidra_facts.
//Usage (headless): analyzeHeadless <proj> <name> -process <prog> -postScript ImportWorkbenchFacts.java <facts.json>
//Usage (GUI): run from the Script Manager; a file chooser asks for the input path.
//Re-running is safe: references and comment lines are updated in place, never duplicated.
//@author Flycast workbench
//@category Workbench
//@keybinding
//@menupath Tools.Workbench.Import Workbench Facts
//@toolbar

import java.io.File;
import java.io.FileReader;
import java.io.Reader;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.BookmarkManager;
import ghidra.program.model.listing.BookmarkType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.SourceType;

public class ImportWorkbenchFacts extends GhidraScript {

    private static final String BOOKMARK_TYPE = BookmarkType.ANALYSIS;
    private static final String BOOKMARK_CATEGORY = "Workbench";
    private static final int HOT_FUNCTION_LIMIT = 50;
    private static final int TARGETS_PER_SITE = 4;

    /** Comment slots we touch; mapped onto the FlatProgramAPI accessors, which are stable across 11.x/12.x. */
    private enum Kind { PLATE, EOL, PRE }

    private int createdFunctions = 0;
    private int createdRefs = 0;
    private int comments = 0;
    private int bookmarks = 0;
    private int skipped = 0;

    @Override
    protected void run() throws Exception {
        File in = pickInputFile();
        if (in == null) {
            printerr("ImportWorkbenchFacts: no input file given; aborting.");
            return;
        }
        JsonObject root;
        try (Reader r = new FileReader(in)) {
            root = JsonParser.parseReader(r).getAsJsonObject();
        }

        monitor.setMessage("workbench: call targets");
        importCallTargets(array(root, "call_targets"));
        monitor.setMessage("workbench: indirect calls");
        importIndirectCalls(array(root, "indirect_calls"));
        monitor.setMessage("workbench: memory writers");
        importMemoryWriters(array(root, "memory_writers"));
        monitor.setMessage("workbench: TA submitters");
        importTaSubmitters(array(root, "ta_submitters"));

        println(String.format(
            "ImportWorkbenchFacts: %d functions created, %d references added, %d comments set, "
                + "%d bookmarks set, %d records skipped (address not in memory)",
            createdFunctions, createdRefs, comments, bookmarks, skipped));
    }

    private File pickInputFile() throws Exception {
        String[] args = getScriptArgs();
        if (args != null && args.length > 0 && !args[0].isBlank()) {
            return new File(args[0]);
        }
        if (isRunningHeadless()) {
            return null;
        }
        return askFile("Workbench facts JSON", "Import");
    }

    // ----------------------------------------------------------------- call_targets

    private void importCallTargets(JsonArray items) throws Exception {
        List<JsonObject> hot = new ArrayList<>();
        for (JsonElement e : items) {
            if (monitor.isCancelled()) {
                return;
            }
            JsonObject o = e.getAsJsonObject();
            Address addr = resolve(getLong(o, "address"));
            if (addr == null) {
                skipped++;
                continue;
            }
            long calls = getLong(o, "calls");
            long callers = getLong(o, "callers");
            ensureFunction(addr);
            String line = String.format("workbench: called %d times from %d sites", calls, callers);
            if (setCommentLine(addr, Kind.PLATE, "workbench: called", line)) {
                comments++;
            }
            hot.add(o);
        }
        hot.sort(Comparator.comparingLong((JsonObject o) -> getLong(o, "calls")).reversed());
        for (int i = 0; i < hot.size() && i < HOT_FUNCTION_LIMIT; i++) {
            JsonObject o = hot.get(i);
            Address addr = resolve(getLong(o, "address"));
            if (addr != null) {
                setBookmark(addr, String.format("hot function: %d calls", getLong(o, "calls")));
            }
        }
    }

    private void ensureFunction(Address addr) {
        if (getFunctionAt(addr) != null) {
            return;
        }
        if (getInstructionAt(addr) == null) {
            disassemble(addr);
        }
        if (getInstructionAt(addr) == null) {
            return; // undecodable bytes: leave the plate comment as the only annotation
        }
        Function f = createFunction(addr, null);
        if (f != null) {
            createdFunctions++;
        }
    }

    // --------------------------------------------------------------- indirect_calls

    private void importIndirectCalls(JsonArray items) {
        // site pc -> list of {target, calls}, preserving first-seen order
        Map<Long, List<long[]>> sites = new LinkedHashMap<>();
        for (JsonElement e : items) {
            JsonObject o = e.getAsJsonObject();
            long site = getLong(o, "address");
            sites.computeIfAbsent(site, k -> new ArrayList<>())
                .add(new long[] { getLong(o, "target_pc"), getLong(o, "calls") });
        }
        ReferenceManager refs = currentProgram.getReferenceManager();
        for (Map.Entry<Long, List<long[]>> entry : sites.entrySet()) {
            if (monitor.isCancelled()) {
                return;
            }
            Address from = resolve(entry.getKey());
            if (from == null) {
                skipped++;
                continue;
            }
            Instruction ins = getInstructionAt(from);
            if (ins == null) {
                disassemble(from);
                ins = getInstructionAt(from);
            }
            RefType type = RefType.COMPUTED_CALL;
            if (ins != null) {
                String mn = ins.getMnemonicString().toLowerCase();
                if (mn.startsWith("jmp") || mn.startsWith("braf")) {
                    type = RefType.COMPUTED_JUMP;
                }
            }
            List<long[]> targets = entry.getValue();
            targets.sort(Comparator.comparingLong((long[] t) -> t[1]).reversed());
            StringBuilder eol = new StringBuilder();
            int listed = 0;
            for (long[] t : targets) {
                Address to = resolve(t[0]);
                if (to == null) {
                    skipped++;
                } else {
                    Reference existing = refs.getReference(from, to, 0);
                    if (existing == null) {
                        refs.addMemoryReference(from, to, type, SourceType.USER_DEFINED, 0);
                        createdRefs++;
                    }
                }
                if (listed < TARGETS_PER_SITE) {
                    eol.append(listed == 0 ? "workbench: -> " : ", ");
                    eol.append(String.format("0x%08x (%d)", t[0] & 0xffffffffL, t[1]));
                    listed++;
                }
            }
            if (listed > 0 && setCommentLine(from, Kind.EOL, "workbench: ->", eol.toString())) {
                comments++;
            }
        }
    }

    // --------------------------------------------------------------- memory_writers

    private void importMemoryWriters(JsonArray items) {
        // site pc -> {min addr, max addr, width (-1 when mixed), total writes}
        Map<Long, long[]> sites = new LinkedHashMap<>();
        for (JsonElement e : items) {
            JsonObject o = e.getAsJsonObject();
            long site = getLong(o, "address");
            long mem = getLong(o, "mem_addr") & 0xffffffffL;
            long width = getLong(o, "mem_width");
            long writes = getLong(o, "writes");
            long[] agg = sites.get(site);
            if (agg == null) {
                sites.put(site, new long[] { mem, mem, width, writes });
            } else {
                agg[0] = Math.min(agg[0], mem);
                agg[1] = Math.max(agg[1], mem);
                if (agg[2] != width) {
                    agg[2] = -1;
                }
                agg[3] += writes;
            }
        }
        for (Map.Entry<Long, long[]> entry : sites.entrySet()) {
            if (monitor.isCancelled()) {
                return;
            }
            Address site = resolve(entry.getKey());
            if (site == null) {
                skipped++;
                continue;
            }
            long[] a = entry.getValue();
            String range = a[0] == a[1]
                ? String.format("0x%08x", a[0])
                : String.format("0x%08x..0x%08x", a[0], a[1]);
            String width = a[2] < 0 ? "mixed width" : a[2] + " bytes";
            String line = String.format("workbench: writes %s (%s) x%d", range, width, a[3]);
            if (setCommentLine(site, Kind.EOL, "workbench: writes", line)) {
                comments++;
            }
        }
    }

    // ---------------------------------------------------------------- ta_submitters

    private void importTaSubmitters(JsonArray items) {
        for (JsonElement e : items) {
            if (monitor.isCancelled()) {
                return;
            }
            JsonObject o = e.getAsJsonObject();
            Address addr = resolve(getLong(o, "address"));
            if (addr == null) {
                skipped++;
                continue;
            }
            String line = String.format("workbench: submits TA geometry (%d blocks)", getLong(o, "blocks"));
            if (setCommentLine(addr, Kind.PRE, "workbench: submits", line)) {
                comments++;
            }
            setBookmark(addr, "TA submitter");
        }
    }

    // --------------------------------------------------------------------- helpers

    /**
     * Map an emulator-side SH-4 address onto this program's memory. Tries the raw value,
     * then the P1-cached alias (0x8c......), then the bare physical address, then the
     * P0/P2 aliases. Returns null when none of them is backed by a memory block.
     */
    private Address resolve(long raw) {
        Memory mem = currentProgram.getMemory();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        long phys = raw & 0x1fffffffL;
        long[] candidates = {
            raw & 0xffffffffL,
            phys | 0x8c000000L,
            phys,
            phys | 0x80000000L,
            phys | 0xa0000000L,
        };
        for (long c : candidates) {
            try {
                Address a = space.getAddress(c);
                if (a != null && mem.contains(a)) {
                    return a;
                }
            } catch (RuntimeException ignored) {
                // AddressOutOfBoundsException for values the space cannot represent
            }
        }
        return null;
    }

    /**
     * Set or update a single line inside a multi-line comment. Any existing line starting
     * with {@code prefix} is replaced (so counts refresh on re-run); other lines are kept.
     * Returns true when the stored comment actually changed.
     */
    private boolean setCommentLine(Address addr, Kind kind, String prefix, String line) {
        String existing = getComment(addr, kind);
        String updated;
        if (existing == null || existing.isEmpty()) {
            updated = line;
        } else {
            String[] lines = existing.split("\n", -1);
            StringBuilder sb = new StringBuilder();
            boolean replaced = false;
            for (String l : lines) {
                if (sb.length() > 0) {
                    sb.append('\n');
                }
                if (!replaced && l.startsWith(prefix)) {
                    sb.append(line);
                    replaced = true;
                } else {
                    sb.append(l);
                }
            }
            if (!replaced) {
                sb.append('\n').append(line);
            }
            updated = sb.toString();
        }
        if (updated.equals(existing)) {
            return false;
        }
        switch (kind) {
            case PLATE -> setPlateComment(addr, updated);
            case EOL -> setEOLComment(addr, updated);
            case PRE -> setPreComment(addr, updated);
        }
        return true;
    }

    private String getComment(Address addr, Kind kind) {
        return switch (kind) {
            case PLATE -> getPlateComment(addr);
            case EOL -> getEOLComment(addr);
            case PRE -> getPreComment(addr);
        };
    }

    /** Create or merge the Workbench bookmark at addr; one bookmark per address, no duplicate text. */
    private void setBookmark(Address addr, String comment) {
        BookmarkManager bm = currentProgram.getBookmarkManager();
        Bookmark existing = bm.getBookmark(addr, BOOKMARK_TYPE, BOOKMARK_CATEGORY);
        if (existing == null) {
            bm.setBookmark(addr, BOOKMARK_TYPE, BOOKMARK_CATEGORY, comment);
            bookmarks++;
            return;
        }
        String current = existing.getComment() == null ? "" : existing.getComment();
        if (current.contains(comment)) {
            return;
        }
        existing.set(BOOKMARK_CATEGORY, current.isEmpty() ? comment : current + "; " + comment);
        bookmarks++;
    }

    private static JsonArray array(JsonObject root, String key) {
        JsonElement e = root.get(key);
        return e == null || !e.isJsonArray() ? new JsonArray() : e.getAsJsonArray();
    }

    private static long getLong(JsonObject o, String key) {
        JsonElement e = o.get(key);
        return e == null || e.isJsonNull() ? 0L : e.getAsLong();
    }
}
