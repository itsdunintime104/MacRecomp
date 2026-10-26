// Exports the evidence that tools/pe/build_names.py turns into recon/names.json.
// Usage: -readOnly -postScript ExportNameEvidence.java <outfile>
// Writes one JSON object:
//   "fid":       FunctionID matches per function (name, library, version, variant, score),
//                from every FunctionID database that ships with this Ghidra.
//   "functions": per function: entry, current Ghidra name and its source type, and every
//                address in a non-executable block that the function body refers to
//                (strings, vtables, globals).
// The program is not modified.
// @category Recomp
import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.db.FidQueryService;
import ghidra.feature.fid.db.LibraryRecord;
import ghidra.feature.fid.service.FidMatch;
import ghidra.feature.fid.service.FidSearchResult;
import ghidra.feature.fid.service.FidService;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

import com.google.gson.Gson;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

public class ExportNameEvidence extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportNameEvidence.java <outfile>");
        }

        FidService service = new FidService();
        List<Map<String, Object>> fidRows = new ArrayList<>();
        try (FidQueryService query = service.openFidQueryService(currentProgram.getLanguage(), false)) {
            List<FidSearchResult> results = service.processProgram(
                currentProgram, query, service.getDefaultScoreThreshold(), monitor);
            for (FidSearchResult result : results) {
                if (result.matches == null || result.matches.isEmpty()) {
                    continue;
                }
                List<Map<String, Object>> matches = new ArrayList<>();
                for (FidMatch match : result.matches) {
                    LibraryRecord lib = match.getLibraryRecord();
                    Map<String, Object> m = new LinkedHashMap<>();
                    m.put("name", match.getFunctionRecord().getName());
                    m.put("lib", lib.getLibraryFamilyName());
                    m.put("ver", lib.getLibraryVersion());
                    m.put("variant", lib.getLibraryVariant());
                    m.put("score", match.getOverallScore());
                    matches.add(m);
                }
                Map<String, Object> row = new LinkedHashMap<>();
                row.put("addr", result.function.getEntryPoint().getOffset());
                row.put("matches", matches);
                fidRows.add(row);
            }
        }

        List<Map<String, Object>> funcRows = new ArrayList<>();
        for (Function func : currentProgram.getFunctionManager().getFunctions(true)) {
            monitor.checkCancelled();
            TreeSet<Long> refs = new TreeSet<>();
            InstructionIterator it = currentProgram.getListing().getInstructions(func.getBody(), true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                for (Reference ref : insn.getReferencesFrom()) {
                    Address to = ref.getToAddress();
                    if (!to.isMemoryAddress()) {
                        continue;
                    }
                    MemoryBlock block = currentProgram.getMemory().getBlock(to);
                    if (block != null && !block.isExecute()) {
                        refs.add(to.getOffset());
                    }
                }
            }
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("addr", func.getEntryPoint().getOffset());
            row.put("name", func.getName(true));
            row.put("src", func.getSymbol().getSource().toString());
            row.put("refs", refs);
            funcRows.add(row);
        }

        Map<String, Object> out = new LinkedHashMap<>();
        out.put("fid", fidRows);
        out.put("functions", funcRows);
        try (PrintWriter w = new PrintWriter(new File(args[0]), StandardCharsets.UTF_8)) {
            new Gson().toJson(out, w);
        }
        println("fid matches: " + fidRows.size() + ", functions: " + funcRows.size());
    }
}
