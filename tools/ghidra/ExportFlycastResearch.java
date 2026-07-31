//@category Flycast Research

import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressRangeIterator;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.data.Array;
import ghidra.program.model.data.Composite;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.Dynamic;
import ghidra.program.model.data.FunctionDefinition;
import ghidra.program.model.data.Pointer;
import ghidra.program.model.data.TypeDef;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportFlycastResearch extends GhidraScript {
  private static final Pattern ID = Pattern.compile("[A-Za-z0-9][A-Za-z0-9._-]{0,127}");
  private static final long ADDRESS_SPACE_SIZE = 1L << 32;
  private static final int MAX_BLOCKS = 4096;
  private static final int MAX_FUNCTIONS = 250000;
  private static final int MAX_SYMBOLS = 1000000;
  private static final int MAX_DATA_TYPES = 100000;
  private static final int MAX_BODY_RANGES = 1000000;
  private static final int MAX_PARAMETERS = 64;
  private static final int MAX_TEXT = 4096;
  private static final int MAX_DEFINITION = 65536;
  private static final int MAX_EXPORT_BYTES = 64 * 1024 * 1024;

  private AddressSpace defaultSpace;

  private static String bounded(String value, String field, int maximum, boolean allowEmpty) {
    if (value == null || (!allowEmpty && value.isEmpty())
        || value.getBytes(StandardCharsets.UTF_8).length > maximum)
      throw new IllegalArgumentException(field + " has an invalid length");
    return value;
  }

  private static int compareUtf8(String left, String right) {
    byte[] leftBytes = left.getBytes(StandardCharsets.UTF_8);
    byte[] rightBytes = right.getBytes(StandardCharsets.UTF_8);
    int shared = Math.min(leftBytes.length, rightBytes.length);
    for (int index = 0; index < shared; ++index) {
      int comparison = Integer.compare(leftBytes[index] & 0xff, rightBytes[index] & 0xff);
      if (comparison != 0)
        return comparison;
    }
    return Integer.compare(leftBytes.length, rightBytes.length);
  }

  private static String hex(byte[] bytes) {
    StringBuilder text = new StringBuilder(bytes.length * 2);
    for (byte value : bytes)
      text.append(String.format(Locale.ROOT, "%02x", value & 0xff));
    return text.toString();
  }

  private static String sha256(InputStream input) throws Exception {
    MessageDigest digest = MessageDigest.getInstance("SHA-256");
    byte[] buffer = new byte[65536];
    try (BufferedInputStream stream = new BufferedInputStream(input)) {
      for (int count; (count = stream.read(buffer)) != -1; )
        digest.update(buffer, 0, count);
    }
    return hex(digest.digest());
  }

  private static String sha256(Path path) throws Exception {
    return sha256(Files.newInputStream(path, StandardOpenOption.READ));
  }

  private static Path importedExecutablePath(String value) {
    String path = value;
    if (System.getProperty("os.name", "").toLowerCase(Locale.ROOT).contains("windows")
        && path.matches("^/[A-Za-z]:/.*"))
      path = path.substring(1);
    return Path.of(path).toAbsolutePath().normalize();
  }

  private long offset(Address address, String field) {
    if (!address.getAddressSpace().equals(defaultSpace))
      throw new IllegalArgumentException(field + " is outside the default address space");
    long value = address.getUnsignedOffset();
    if (value < 0 || value >= ADDRESS_SPACE_SIZE)
      throw new IllegalArgumentException(field + " is outside the 32-bit address space");
    return value;
  }

  private static String address(long value) {
    if (value < 0 || value >= ADDRESS_SPACE_SIZE)
      throw new IllegalArgumentException("address is outside the 32-bit address space");
    return String.format(Locale.ROOT, "0x%08x", value);
  }

  private static JsonObject range(long start, long length) {
    if (length <= 0 || start < 0 || start >= ADDRESS_SPACE_SIZE
        || length > ADDRESS_SPACE_SIZE - start)
      throw new IllegalArgumentException("range is empty or wraps the address space");
    JsonObject result = new JsonObject();
    result.addProperty("start_address", address(start));
    result.addProperty("length", length);
    return result;
  }

  private static boolean rangeContained(JsonArray blocks, long start, long end,
      boolean requireInitialized, boolean requireExecutable) {
    for (JsonElement element : blocks) {
      JsonObject block = element.getAsJsonObject();
      long blockStart = Long.decode(block.get("start_address").getAsString());
      long blockEnd = blockStart + block.get("length").getAsLong();
      if (start >= blockStart && end <= blockEnd
          && (!requireInitialized || block.get("initialized").getAsBoolean())
          && (!requireExecutable || block.get("execute").getAsBoolean()))
        return true;
    }
    return false;
  }

  private static void validateFunctions(JsonArray functions, JsonArray blocks) {
    long previousEntry = -1;
    List<long[]> allRanges = new ArrayList<>();
    for (JsonElement element : functions) {
      JsonObject function = element.getAsJsonObject();
      long entry = Long.decode(function.get("entry_address").getAsString());
      if ((entry & 1) != 0 || entry <= previousEntry)
        throw new IllegalArgumentException("functions are not uniquely ordered by even entry");
      previousEntry = entry;
      JsonArray ranges = function.getAsJsonArray("body_ranges");
      if (ranges.isEmpty())
        throw new IllegalArgumentException("function body is empty at " + address(entry));
      long previousEnd = -1;
      boolean entryCovered = false;
      for (JsonElement rangeElement : ranges) {
        JsonObject body = rangeElement.getAsJsonObject();
        long start = Long.decode(body.get("start_address").getAsString());
        long length = body.get("length").getAsLong();
        long end = start + length;
        if ((start & 1) != 0 || (length & 1) != 0 || start < previousEnd)
          throw new IllegalArgumentException("function body is not canonical at "
              + address(entry));
        if (!rangeContained(blocks, start, end, true, true))
          throw new IllegalArgumentException("function body is outside initialized executable "
              + "memory at " + address(entry));
        entryCovered |= entry >= start && entry < end;
        previousEnd = end;
        allRanges.add(new long[] { start, end });
      }
      if (!entryCovered)
        throw new IllegalArgumentException("function entry is outside its body at "
            + address(entry));
    }
    allRanges.sort((left, right) -> {
      int byStart = Long.compare(left[0], right[0]);
      return byStart != 0 ? byStart : Long.compare(left[1], right[1]);
    });
    for (int index = 1; index < allRanges.size(); ++index) {
      if (allRanges.get(index)[0] < allRanges.get(index - 1)[1])
        throw new IllegalArgumentException("function bodies overlap");
    }
  }

  private static String namespace(Symbol symbol) {
    return bounded(symbol.getParentNamespace().getName(true), "symbol namespace", MAX_TEXT, true);
  }

  private static String namespace(Function function) {
    return bounded(function.getParentNamespace().getName(true), "function namespace", MAX_TEXT,
        true);
  }

  private static String normalizedIdentifier(String value, String field) {
    String normalized = bounded(value, field, 128, false).toLowerCase(Locale.ROOT)
        .replace(' ', '_');
    if (!ID.matcher(normalized).matches())
      throw new IllegalArgumentException(field + " is not a portable identifier: " + value);
    return normalized;
  }

  private static String typeKind(DataType type) {
    if (type instanceof Array)
      return "array";
    if (type instanceof Composite)
      return "composite";
    if (type instanceof ghidra.program.model.data.Enum)
      return "enum";
    if (type instanceof FunctionDefinition)
      return "function";
    if (type instanceof Pointer)
      return "pointer";
    if (type instanceof TypeDef)
      return "typedef";
    return "other";
  }

  @Override
  public void run() throws Exception {
    String[] arguments = getScriptArgs();
    if (arguments.length != 2)
      throw new IllegalArgumentException(
          "usage: ExportFlycastResearch.java <output.json> <export-id>");
    Path output = Path.of(arguments[0]).toAbsolutePath().normalize();
    String exportId = arguments[1];
    if (!ID.matcher(exportId).matches())
      throw new IllegalArgumentException("export-id is not portable");
    if (Files.exists(output))
      throw new IllegalArgumentException("output already exists: " + output);
    if (output.getParent() == null || !Files.isDirectory(output.getParent()))
      throw new IllegalArgumentException("output parent does not exist: " + output);

    Path executable = importedExecutablePath(currentProgram.getExecutablePath());
    if (!Files.isRegularFile(executable))
      throw new IllegalArgumentException("program executable path is not a regular file");
    long executableSize = Files.size(executable);
    if (executableSize <= 0)
      throw new IllegalArgumentException("program executable is empty");
    String executableDigest = sha256(executable);
    String exporterDigest = sha256(getSourceFile().getInputStream());

    defaultSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();
    if (!currentProgram.getLanguageID().toString().equals("SuperH4:LE:32:default"))
      throw new IllegalArgumentException("program does not use SuperH4:LE:32:default");

    List<JsonObject> blocks = new ArrayList<>();
    List<long[]> blockKeys = new ArrayList<>();
    for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
      if (!block.getStart().getAddressSpace().equals(defaultSpace))
        continue;
      long start = offset(block.getStart(), "memory block start");
      long length = block.getSize();
      if (length <= 0 || length > ADDRESS_SPACE_SIZE - start)
        throw new IllegalArgumentException("memory block is empty or wrapping: " + block.getName());
      JsonObject item = range(start, length);
      item.addProperty("name", bounded(block.getName(), "memory block name", MAX_TEXT, false));
      item.addProperty("read", block.isRead());
      item.addProperty("write", block.isWrite());
      item.addProperty("execute", block.isExecute());
      item.addProperty("initialized", block.isInitialized());
      blocks.add(item);
      blockKeys.add(new long[] { start, blocks.size() - 1 });
      if (blocks.size() > MAX_BLOCKS)
        throw new IllegalArgumentException("memory block count exceeds " + MAX_BLOCKS);
    }
    blockKeys.sort((left, right) -> {
      int byStart = Long.compare(left[0], right[0]);
      if (byStart != 0)
        return byStart;
      return compareUtf8(blocks.get((int) left[1]).get("name").getAsString(),
          blocks.get((int) right[1]).get("name").getAsString());
    });
    JsonArray blockArray = new JsonArray();
    for (long[] key : blockKeys)
      blockArray.add(blocks.get((int) key[1]));
    if (blockArray.isEmpty())
      throw new IllegalArgumentException("program has no default-space memory blocks");

    List<JsonObject> functions = new ArrayList<>();
    FunctionIterator functionIterator = currentProgram.getFunctionManager().getFunctions(true);
    int bodyRangeCount = 0;
    while (functionIterator.hasNext()) {
      Function function = functionIterator.next();
      if (function.isExternal()
          || !function.getEntryPoint().getAddressSpace().equals(defaultSpace))
        continue;
      JsonObject item = new JsonObject();
      item.addProperty("entry_address", address(offset(function.getEntryPoint(), "function entry")));
      item.addProperty("name", bounded(function.getName(), "function name", MAX_TEXT, false));
      item.addProperty("namespace", namespace(function));
      String convention = function.getCallingConventionName();
      item.addProperty("calling_convention",
          bounded(convention == null ? "unknown" : convention, "calling convention", 256, false));
      item.addProperty("return_type",
          bounded(function.getReturnType().getPathName(), "return type", MAX_TEXT, false));
      JsonArray parameterTypes = new JsonArray();
      Parameter[] parameters = function.getParameters();
      if (parameters.length > MAX_PARAMETERS)
        throw new IllegalArgumentException("function parameter count exceeds " + MAX_PARAMETERS);
      for (Parameter parameter : parameters)
        parameterTypes.add(bounded(parameter.getDataType().getPathName(), "parameter type",
            MAX_TEXT, false));
      item.add("parameter_types", parameterTypes);
      JsonArray bodyRanges = new JsonArray();
      AddressRangeIterator ranges = function.getBody().getAddressRanges(true);
      while (ranges.hasNext()) {
        AddressRange body = ranges.next();
        long start = offset(body.getMinAddress(), "function body start");
        bodyRanges.add(range(start, body.getLength()));
        if (++bodyRangeCount > MAX_BODY_RANGES)
          throw new IllegalArgumentException("function body range count exceeds " + MAX_BODY_RANGES);
      }
      item.add("body_ranges", bodyRanges);
      item.addProperty("thunk", function.isThunk());
      item.addProperty("no_return", function.hasNoReturn());
      functions.add(item);
      if (functions.size() > MAX_FUNCTIONS)
        throw new IllegalArgumentException("function count exceeds " + MAX_FUNCTIONS);
    }
    functions.sort(Comparator.comparing(item -> item.get("entry_address").getAsString()));
    JsonArray functionArray = new JsonArray();
    for (JsonObject function : functions)
      functionArray.add(function);
    if (functionArray.isEmpty())
      throw new IllegalArgumentException("program has no internal functions");
    validateFunctions(functionArray, blockArray);

    List<JsonObject> symbols = new ArrayList<>();
    SymbolIterator symbolIterator = currentProgram.getSymbolTable().getAllSymbols(true);
    while (symbolIterator.hasNext()) {
      Symbol symbol = symbolIterator.next();
      Address symbolAddress = symbol.getAddress();
      if (!symbolAddress.isMemoryAddress() || !symbolAddress.getAddressSpace().equals(defaultSpace))
        continue;
      if (!currentProgram.getMemory().contains(symbolAddress))
        continue;
      JsonObject item = new JsonObject();
      item.addProperty("address", address(offset(symbolAddress, "symbol address")));
      item.addProperty("name", bounded(symbol.getName(), "symbol name", MAX_TEXT, false));
      item.addProperty("namespace", namespace(symbol));
      item.addProperty("kind", normalizedIdentifier(symbol.getSymbolType().toString(),
          "symbol kind"));
      item.addProperty("source", normalizedIdentifier(symbol.getSource().toString(),
          "symbol source"));
      item.addProperty("primary", symbol.isPrimary());
      symbols.add(item);
      if (symbols.size() > MAX_SYMBOLS)
        throw new IllegalArgumentException("symbol count exceeds " + MAX_SYMBOLS);
    }
    symbols.sort((left, right) -> {
      int comparison = left.get("address").getAsString()
          .compareTo(right.get("address").getAsString());
      if (comparison == 0)
        comparison = compareUtf8(left.get("kind").getAsString(),
            right.get("kind").getAsString());
      if (comparison == 0)
        comparison = compareUtf8(left.get("namespace").getAsString(),
            right.get("namespace").getAsString());
      if (comparison == 0)
        comparison = compareUtf8(left.get("name").getAsString(),
            right.get("name").getAsString());
      return comparison;
    });
    JsonArray symbolArray = new JsonArray();
    for (JsonObject symbol : symbols)
      symbolArray.add(symbol);
    if (symbolArray.isEmpty())
      throw new IllegalArgumentException("program has no memory symbols");

    List<JsonObject> dataTypes = new ArrayList<>();
    Iterator<DataType> typeIterator = currentProgram.getDataTypeManager().getAllDataTypes();
    while (typeIterator.hasNext()) {
      DataType type = typeIterator.next();
      JsonObject item = new JsonObject();
      item.addProperty("path", bounded(type.getPathName(), "data type path", MAX_TEXT, false));
      item.addProperty("kind", typeKind(type));
      int length = type.getLength();
      item.addProperty("length", Math.max(length, 0));
      item.addProperty("dynamic", type instanceof Dynamic || length < 0);
      String definition = type.toString();
      if (definition == null || definition.isEmpty())
        definition = type.getDisplayName();
      item.addProperty("definition",
          bounded(definition, "data type definition", MAX_DEFINITION, false));
      dataTypes.add(item);
      if (dataTypes.size() > MAX_DATA_TYPES)
        throw new IllegalArgumentException("data type count exceeds " + MAX_DATA_TYPES);
    }
    dataTypes.sort((left, right) -> compareUtf8(left.get("path").getAsString(),
        right.get("path").getAsString()));
    JsonArray typeArray = new JsonArray();
    for (JsonObject type : dataTypes)
      typeArray.add(type);

    long ghidraImageBase = offset(currentProgram.getImageBase(), "Ghidra image base");
    long imageBase = ghidraImageBase;
    String imageBaseSource = "ghidra-program";
    boolean ghidraBaseIsExecutable = false;
    long firstInitializedExecutable = ADDRESS_SPACE_SIZE;
    for (JsonElement element : blockArray) {
      JsonObject block = element.getAsJsonObject();
      long start = Long.decode(block.get("start_address").getAsString());
      long end = start + block.get("length").getAsLong();
      if (block.get("initialized").getAsBoolean() && block.get("execute").getAsBoolean()) {
        firstInitializedExecutable = Math.min(firstInitializedExecutable, start);
        if (ghidraImageBase >= start && ghidraImageBase < end)
          ghidraBaseIsExecutable = true;
      }
    }
    if (!ghidraBaseIsExecutable) {
      if (firstInitializedExecutable == ADDRESS_SPACE_SIZE)
        throw new IllegalArgumentException("program has no initialized executable memory block");
      imageBase = firstInitializedExecutable;
      imageBaseSource = "minimum-initialized-executable-block";
    }
    JsonObject firstBlock = blockArray.get(0).getAsJsonObject();
    JsonObject lastBlock = blockArray.get(blockArray.size() - 1).getAsJsonObject();
    long minimum = Long.decode(firstBlock.get("start_address").getAsString());
    long lastStart = Long.decode(lastBlock.get("start_address").getAsString());
    long maximum = lastStart + lastBlock.get("length").getAsLong() - 1;

    JsonObject root = new JsonObject();
    root.addProperty("schema", "flycast-research-ghidra-export");
    root.addProperty("schema_version", 1);
    root.addProperty("export_id", exportId);
    root.addProperty("evidence_class", "static-analysis");
    JsonObject producer = new JsonObject();
    producer.addProperty("name", "Ghidra");
    producer.addProperty("version", getGhidraVersion());
    producer.addProperty("exporter_id", "flycast-ghidra-export-v1");
    producer.addProperty("exporter_sha256", exporterDigest);
    root.add("producer", producer);
    JsonObject program = new JsonObject();
    program.addProperty("name", bounded(currentProgram.getName(), "program name", MAX_TEXT, false));
    program.addProperty("executable_sha256", executableDigest);
    program.addProperty("executable_size", executableSize);
    program.addProperty("language_id", currentProgram.getLanguageID().toString());
    program.addProperty("compiler_spec_id", currentProgram.getCompilerSpec()
        .getCompilerSpecID().toString());
    program.addProperty("endian", currentProgram.getLanguage().isBigEndian() ? "big" : "little");
    program.addProperty("address_size", defaultSpace.getSize());
    program.addProperty("address_space", defaultSpace.getName());
    program.addProperty("ghidra_image_base", address(ghidraImageBase));
    program.addProperty("image_base", address(imageBase));
    program.addProperty("image_base_source", imageBaseSource);
    program.addProperty("minimum_address", address(minimum));
    program.addProperty("maximum_address", address(maximum));
    root.add("program", program);
    root.add("memory_blocks", blockArray);
    root.add("functions", functionArray);
    root.add("symbols", symbolArray);
    root.add("data_types", typeArray);
    JsonObject counts = new JsonObject();
    counts.addProperty("memory_blocks", blockArray.size());
    counts.addProperty("functions", functionArray.size());
    counts.addProperty("symbols", symbolArray.size());
    counts.addProperty("data_types", typeArray.size());
    root.add("counts", counts);

    Gson gson = new GsonBuilder().disableHtmlEscaping().setPrettyPrinting().create();
    byte[] bytes = (gson.toJson(root) + "\n").getBytes(StandardCharsets.UTF_8);
    if (bytes.length > MAX_EXPORT_BYTES)
      throw new IllegalArgumentException("export exceeds " + MAX_EXPORT_BYTES + " bytes");
    try {
      Files.write(output, bytes, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
    }
    catch (IOException exception) {
      Files.deleteIfExists(output);
      throw exception;
    }
    println("Exported " + functionArray.size() + " functions, " + symbolArray.size()
        + " symbols, and " + typeArray.size() + " data types to " + output);
  }
}
