// Decompiles a list of functions (names or 0xADDRESS, one per line) and lists
// each one's callers. Copied from the Quest project's ghidra\DecompileList.java;
// here a missing function at an address is created first (vtable targets the
// auto-analysis did not reach).
// Usage: -postScript DecompileList.java <names-file> <output-file>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

public class DecompileList extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> names = Files.readAllLines(Paths.get(args[0]));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[1], "UTF-8")) {
            for (String raw : names) {
                String name = raw.trim();
                if (name.isEmpty() || name.startsWith("#")) continue;
                Function f = null;
                if (name.startsWith("0x")) {
                    // "0xADDR" = the function containing ADDR; "0xADDR!" also
                    // creates one at ADDR when none covers it (a vtable target,
                    // never a return address)
                    boolean create = name.endsWith("!");
                    Address a = toAddr(Long.parseLong(name.substring(2, create ? name.length() - 1 : name.length()), 16));
                    f = getFunctionContaining(a);
                    if (f == null && create) {
                        disassemble(a);
                        f = createFunction(a, null);
                    }
                } else {
                    for (Symbol s : currentProgram.getSymbolTable().getSymbols(name)) {
                        f = getFunctionAt(s.getAddress());
                        if (f != null) break;
                    }
                }
                out.println("==================================================================");
                if (f == null) {
                    out.println("// NOT FOUND: " + name);
                    continue;
                }
                out.println("// " + name + " @ " + f.getEntryPoint());
                StringBuilder callers = new StringBuilder();
                int n = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (++n > 40) { callers.append("\n//   ..."); break; }
                    Function c = getFunctionContaining(r.getFromAddress());
                    callers.append("\n//   ").append(r.getFromAddress()).append(" ").append(r.getReferenceType())
                           .append(" ").append(c == null ? "?" : c.getName(true));
                }
                out.println("// refs:" + callers);
                DecompileResults res = di.decompileFunction(f, 120, monitor);
                out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// decompile failed");
            }
        }
    }
}
