// Exports every computed jump and the targets Ghidra resolved for it.
// Usage: -postScript ExportJumpTables.java <outdir>
// Writes <outdir>/recon/ghidra_jumptables.json
//
// MSVC compiles a switch to `jmp dword ptr [table + idx*4]`, sometimes behind a
// byte index map. The engine's sweeper only recognises the `jmp reg` shape that
// position-independent Mach-O code uses, so the table addresses and targets are
// taken from Ghidra's own jump-table analysis instead.
// @category Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ExportJumpTables extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: ExportJumpTables.java <outdir>");
        }
        File reconDir = new File(args[0], "recon");
        reconDir.mkdirs();

        List<Map<String, Object>> rows = new ArrayList<>();
        int unresolved = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction insn = it.next();
            if (!insn.getFlowType().isJump() || !insn.getFlowType().isComputed()) {
                continue;
            }
            Address[] flows = insn.getFlows();
            if (flows == null || flows.length == 0) {
                unresolved++;
                continue;
            }
            // The data reference on a computed jump is the jump table itself.
            long tableAddr = 0;
            for (Reference ref : insn.getReferencesFrom()) {
                RefType t = ref.getReferenceType();
                if (t.isData() || t == RefType.READ) {
                    tableAddr = ref.getToAddress().getOffset();
                    break;
                }
            }
            List<Long> targets = new ArrayList<>();
            for (Address a : flows) {
                targets.add(a.getOffset());
            }
            Function f = getFunctionContaining(insn.getAddress());
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("jmp_addr", insn.getAddress().getOffset());
            row.put("pic_base", 0);
            row.put("table_addr", tableAddr);
            row.put("count", targets.size());
            row.put("targets", targets);
            row.put("kind", "abs");
            row.put("func_addr", f == null ? 0 : f.getEntryPoint().getOffset());
            row.put("mnemonic", insn.toString());
            rows.add(row);
        }

        Gson gson = new GsonBuilder().disableHtmlEscaping().create();
        try (PrintWriter w = new PrintWriter(new File(reconDir, "ghidra_jumptables.json"),
                                            StandardCharsets.UTF_8)) {
            w.println(gson.toJson(rows));
        }
        println("ExportJumpTables resolved: " + rows.size() + " unresolved: " + unresolved);
    }
}
