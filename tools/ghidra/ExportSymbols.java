//Export functions, non-default labels and named data to a JSON file so the Flycast
//workbench (tools/workbench) can attach names to runtime addresses recorded by the emulator.
//Usage (headless): analyzeHeadless <proj> <name> -process <prog> -postScript ExportSymbols.java <out.json>
//Usage (GUI): run from the Script Manager; a file chooser asks for the output path.
//@author Flycast workbench
//@category Workbench
//@keybinding
//@menupath Tools.Workbench.Export Symbols
//@toolbar

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.Writer;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolType;

public class ExportSymbols extends GhidraScript {

    /** One exported row; kept as a plain class so sorting is trivial. */
    private static final class Row {
        final long address;
        final String name;
        final long size;
        final String kind;
        final String namespace;

        Row(long address, String name, long size, String kind, String namespace) {
            this.address = address;
            this.name = name;
            this.size = size;
            this.kind = kind;
            this.namespace = namespace;
        }

        JsonObject toJson() {
            JsonObject o = new JsonObject();
            o.addProperty("address", address);
            o.addProperty("name", name);
            o.addProperty("size", size);
            o.addProperty("kind", kind);
            o.addProperty("namespace", namespace);
            return o;
        }
    }

    @Override
    protected void run() throws Exception {
        File out = pickOutputFile();
        if (out == null) {
            printerr("ExportSymbols: no output file given; aborting.");
            return;
        }

        Listing listing = currentProgram.getListing();
        SymbolTable symtab = currentProgram.getSymbolTable();
        List<Row> rows = new ArrayList<>();
        // (address, name) pairs already emitted, so the label pass does not repeat data/functions.
        Set<String> seen = new HashSet<>();

        int functions = exportFunctions(listing, rows, seen);
        int data = exportData(listing, rows, seen);
        int labels = exportLabels(listing, symtab, rows, seen);

        rows.sort(Comparator.comparingLong((Row r) -> r.address).thenComparing(r -> r.name));

        JsonArray symbols = new JsonArray();
        for (Row r : rows) {
            symbols.add(r.toJson());
        }
        JsonObject root = new JsonObject();
        root.addProperty("program", currentProgram.getName());
        root.addProperty("image_base", currentProgram.getImageBase().getOffset());
        root.add("symbols", symbols);

        Gson gson = new GsonBuilder().disableHtmlEscaping().create();
        try (Writer w = new FileWriter(out)) {
            gson.toJson(root, w);
        }

        println(String.format("ExportSymbols: wrote %d symbols (%d functions, %d labels, %d data) to %s",
            rows.size(), functions, labels, data, out.getAbsolutePath()));
    }

    /** Script argument 0 wins (headless); otherwise ask interactively. */
    private File pickOutputFile() throws Exception {
        String[] args = getScriptArgs();
        if (args != null && args.length > 0 && !args[0].isBlank()) {
            File f = new File(args[0]);
            File parent = f.getAbsoluteFile().getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw new IOException("cannot create directory " + parent);
            }
            return f;
        }
        if (isRunningHeadless()) {
            return null;
        }
        return askFile("Export symbols to JSON", "Save");
    }

    private int exportFunctions(Listing listing, List<Row> rows, Set<String> seen) {
        int count = 0;
        FunctionIterator it = listing.getFunctions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            if (f.isExternal()) {
                continue;
            }
            long addr = f.getEntryPoint().getOffset();
            long size = f.getBody() == null ? 0 : f.getBody().getNumAddresses();
            rows.add(new Row(addr, f.getName(), size, "function", namespaceOf(f.getSymbol())));
            seen.add(key(addr, f.getName()));
            count++;
        }
        return count;
    }

    private int exportData(Listing listing, List<Row> rows, Set<String> seen) {
        int count = 0;
        DataIterator it = listing.getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            Symbol sym = d.getPrimarySymbol();
            if (sym == null || isDefaultSymbol(sym)) {
                continue;
            }
            long addr = d.getAddress().getOffset();
            rows.add(new Row(addr, sym.getName(), d.getLength(), "data", namespaceOf(sym)));
            seen.add(key(addr, sym.getName()));
            count++;
        }
        return count;
    }

    private int exportLabels(Listing listing, SymbolTable symtab, List<Row> rows, Set<String> seen) {
        int count = 0;
        // false = exclude dynamic (auto-generated, unstored) symbols.
        SymbolIterator it = symtab.getAllSymbols(false);
        while (it.hasNext() && !monitor.isCancelled()) {
            Symbol sym = it.next();
            if (sym.getSymbolType() != SymbolType.LABEL || sym.isExternal() || isDefaultSymbol(sym)) {
                continue;
            }
            Address a = sym.getAddress();
            if (a == null || !a.isMemoryAddress()) {
                continue;
            }
            // Labels sitting exactly on a function entry are part of the function export.
            if (listing.getFunctionAt(a) != null) {
                continue;
            }
            String k = key(a.getOffset(), sym.getName());
            if (!seen.add(k)) {
                continue; // already exported as the primary symbol of defined data
            }
            rows.add(new Row(a.getOffset(), sym.getName(), 0, "label", namespaceOf(sym)));
            count++;
        }
        return count;
    }

    private static boolean isDefaultSymbol(Symbol sym) {
        return sym.getSource() == SourceType.DEFAULT || sym.isDynamic();
    }

    private static String namespaceOf(Symbol sym) {
        if (sym == null) {
            return "";
        }
        Namespace ns = sym.getParentNamespace();
        if (ns == null || ns.isGlobal()) {
            return "";
        }
        return ns.getName(true);
    }

    private static String key(long addr, String name) {
        return Long.toHexString(addr) + ":" + name;
    }
}
