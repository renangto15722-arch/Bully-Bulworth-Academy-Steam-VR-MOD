// Prints how many functions the analysis found and how much code they cover.
// Usage: -postScript CountFunctions.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class CountFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        long n = 0, bytes = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            n++;
            bytes += f.getBody().getNumAddresses();
        }
        println("FUNCTIONS " + n + " BYTES " + bytes);
    }
}
