// Lists analysis options, enables aggressive function discovery, and re-runs analysis.
// Usage: -postScript ReanalyzePE.java list            (print options only)
//        -postScript ReanalyzePE.java run              (enable options, re-analyze)
// @category Recomp
import ghidra.app.script.GhidraScript;
import ghidra.app.plugin.core.analysis.AutoAnalysisManager;

import java.util.Map;
import java.util.TreeMap;

public class ReanalyzePE extends GhidraScript {

    // Options set to "true" in run mode. Names come from the "list" output.
    private static final String[] ENABLE = {
        "Aggressive Instruction Finder",
    };

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        String mode = args.length > 0 ? args[0] : "list";
        Map<String, String> opts = new TreeMap<>(getCurrentAnalysisOptionsAndValues(currentProgram));
        for (Map.Entry<String, String> e : opts.entrySet()) {
            println("OPT " + e.getKey() + " = " + e.getValue());
        }
        if (!mode.equals("run")) {
            return;
        }
        for (String k : ENABLE) {
            String old = opts.get(k);
            if (old == null) {
                println("MISSING option " + k);
                continue;
            }
            setAnalysisOption(currentProgram, k, "true");
            println("SET " + k + " : " + old + " -> true");
        }
        AutoAnalysisManager mgr = AutoAnalysisManager.getAnalysisManager(currentProgram);
        mgr.reAnalyzeAll(null);
        mgr.startAnalysis(monitor);
        println("ReanalyzePE: analysis finished, functions = "
                + currentProgram.getFunctionManager().getFunctionCount());
    }
}
