// Exports function boundaries and non-code ranges from an analyzed PE32 program.
// Usage: -postScript ExportPE.java <outdir>
// Writes <outdir>/recon/ghidra_functions.json and <outdir>/recon/ghidra_text_data.json
// @category Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ExportPE extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportPE.java <outdir>");
        }
        File reconDir = new File(args[0], "recon");
        reconDir.mkdirs();
        Gson gson = new GsonBuilder().disableHtmlEscaping().create();

        List<Map<String, Object>> rows = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        while (it.hasNext()) {
            Function f = it.next();
            AddressSetView body = f.getBody();
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("addr", f.getEntryPoint().getOffset());
            row.put("name", f.getName());
            row.put("size", body.getNumAddresses());
            row.put("min", body.getMinAddress().getOffset());
            row.put("max", body.getMaxAddress().getOffset());
            List<long[]> ranges = new ArrayList<>();
            for (AddressRange r : body) {
                ranges.add(new long[] {r.getMinAddress().getOffset(), r.getMaxAddress().getOffset()});
            }
            row.put("ranges", ranges);
            row.put("is_thunk", f.isThunk());
            row.put("no_return", f.hasNoReturn());
            row.put("calling_convention", f.getCallingConventionName());
            row.put("param_count", f.getParameterCount());
            rows.add(row);
        }
        try (PrintWriter w = new PrintWriter(new File(reconDir, "ghidra_functions.json"), StandardCharsets.UTF_8)) {
            w.println(gson.toJson(rows));
        }
        println("ExportPE functions: " + rows.size());

        // Defined data that lives inside executable blocks: switch tables and literals
        // the instruction sweeper must not decode as code.
        List<Map<String, Object>> dataRows = new ArrayList<>();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (!b.isExecute()) {
                continue;
            }
            DataIterator di = currentProgram.getListing().getDefinedData(
                    currentProgram.getAddressFactory().getAddressSet(b.getStart(), b.getEnd()), true);
            while (di.hasNext()) {
                Data d = di.next();
                Map<String, Object> row = new LinkedHashMap<>();
                row.put("addr", d.getAddress().getOffset());
                row.put("length", d.getLength());
                row.put("type", d.getDataType().getName());
                row.put("block", b.getName());
                dataRows.add(row);
            }
        }
        try (PrintWriter w = new PrintWriter(new File(reconDir, "ghidra_text_data.json"), StandardCharsets.UTF_8)) {
            w.println(gson.toJson(dataRows));
        }
        println("ExportPE data-in-code items: " + dataRows.size());

        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(String.format("block %s 0x%x-0x%x x=%b w=%b", b.getName(),
                    b.getStart().getOffset(), b.getEnd().getOffset(), b.isExecute(), b.isWrite()));
        }
    }
}
