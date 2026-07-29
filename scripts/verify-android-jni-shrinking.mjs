#!/usr/bin/env node

import fs from "node:fs";

const bridgeClass = "com.nouprax.markdown.core.JvmNative";
const bridgeDescriptor = "Lcom/nouprax/markdown/core/JvmNative;";

const [
    rulesPath,
    usedMappingPath,
    usedConfigurationPath,
    unusedConfigurationPath,
    nativeExportMapPath,
    usedDexDirectory,
    unusedDexDirectory,
    ...extra
] = process.argv.slice(2);
if (
    rulesPath === undefined ||
    usedMappingPath === undefined ||
    usedConfigurationPath === undefined ||
    unusedConfigurationPath === undefined ||
    nativeExportMapPath === undefined ||
    usedDexDirectory === undefined ||
    unusedDexDirectory === undefined ||
    extra.length !== 0
) {
    throw new Error(
        "usage: verify-android-jni-shrinking.mjs " +
            "<published-rules> <used-mapping> <used-configuration> " +
            "<unused-configuration> <native-export-map> <used-dex-directory> " +
            "<unused-dex-directory>"
    );
}

const nativeExportPrefix = "Java_com_nouprax_markdown_core_JvmNative_";
const expectedNativeMethods = new Set(
    [
        ...fs
            .readFileSync(nativeExportMapPath, "utf8")
            .matchAll(/^\s*Java_com_nouprax_markdown_core_JvmNative_([A-Za-z0-9]+);$/gmu)
    ].map((match) => match[1])
);
if (expectedNativeMethods.size !== 13) {
    throw new Error(
        `${nativeExportMapPath} must define exactly 13 ${nativeExportPrefix} exports; ` +
            `found ${expectedNativeMethods.size}`
    );
}

const rules = fs.readFileSync(rulesPath, "utf8");
const expectedRule =
    /-keepclasseswithmembernames,allowoptimization\s+class\s+com\.nouprax\.markdown\.core\.JvmNative\s*\{\s*native\s+<methods>;\s*\}/u;
if (!expectedRule.test(rules)) {
    throw new Error("published Android consumer rules do not protect the exact JvmNative JNI boundary");
}
if (/class\s+\*\s*\{\s*native\s+<methods>;/u.test(rules)) {
    throw new Error("published Android consumer rules contain a broad JNI keep");
}

for (const configurationPath of [usedConfigurationPath, unusedConfigurationPath]) {
    const configuration = fs.readFileSync(configurationPath, "utf8");
    if (!expectedRule.test(configuration)) {
        throw new Error(`R8 did not consume the JvmNative rule from the published AAR: ${configurationPath}`);
    }
    if (/class\s+\*\s*\{\s*native\s+<methods>;/u.test(configuration)) {
        throw new Error(
            "the Android release consumer inherited a broad default JNI rule; " +
                `the library rule is not being tested independently: ${configurationPath}`
        );
    }
}

const mapping = fs.readFileSync(usedMappingPath, "utf8");
const classMappings = mapping.split("\n").filter((line) => line.startsWith(`${bridgeClass} -> `));
if (classMappings.length !== 1 || classMappings[0] !== `${bridgeClass} -> ${bridgeClass}:`) {
    throw new Error(`R8 did not preserve the JNI bridge binary name: ${classMappings.join(", ")}`);
}

function readUleb128(bytes, start) {
    let value = 0;
    let shift = 0;
    let offset = start;
    for (let byteCount = 0; byteCount < 5; byteCount += 1) {
        if (offset >= bytes.length) {
            throw new Error("truncated DEX ULEB128 value");
        }
        const byte = bytes[offset];
        offset += 1;
        value |= (byte & 0x7f) << shift;
        if ((byte & 0x80) === 0) {
            return { value: value >>> 0, offset };
        }
        shift += 7;
    }
    throw new Error("invalid DEX ULEB128 value");
}

function assertRange(bytes, offset, size, label) {
    if (offset < 0 || size < 0 || offset + size > bytes.length) {
        throw new Error(`invalid DEX ${label} range`);
    }
}

function readDexBridge(bytes, path) {
    if (bytes.length < 112 || bytes.subarray(0, 4).toString("ascii") !== "dex\n") {
        throw new Error(`${path} is not a DEX file`);
    }

    const section = (sizeOffset, dataOffset, itemSize, label) => {
        const size = bytes.readUInt32LE(sizeOffset);
        const offset = bytes.readUInt32LE(dataOffset);
        assertRange(bytes, offset, size * itemSize, label);
        return { size, offset };
    };
    const stringIds = section(0x38, 0x3c, 4, "string_ids");
    const typeIds = section(0x40, 0x44, 4, "type_ids");
    const methodIds = section(0x58, 0x5c, 8, "method_ids");
    const classDefs = section(0x60, 0x64, 32, "class_defs");

    const strings = [];
    for (let index = 0; index < stringIds.size; index += 1) {
        const dataOffset = bytes.readUInt32LE(stringIds.offset + index * 4);
        assertRange(bytes, dataOffset, 1, "string_data");
        const payloadOffset = readUleb128(bytes, dataOffset).offset;
        const end = bytes.indexOf(0, payloadOffset);
        if (end < 0) {
            throw new Error("unterminated DEX string_data item");
        }
        strings.push(bytes.subarray(payloadOffset, end).toString("utf8"));
    }

    const typeDescriptors = [];
    for (let index = 0; index < typeIds.size; index += 1) {
        const stringIndex = bytes.readUInt32LE(typeIds.offset + index * 4);
        if (stringIndex >= strings.length) {
            throw new Error("invalid DEX type string index");
        }
        typeDescriptors.push(strings[stringIndex]);
    }

    const methodName = (methodIndex) => {
        if (methodIndex >= methodIds.size) {
            throw new Error("invalid DEX method index");
        }
        const itemOffset = methodIds.offset + methodIndex * 8;
        const classIndex = bytes.readUInt16LE(itemOffset);
        const nameIndex = bytes.readUInt32LE(itemOffset + 4);
        if (
            classIndex >= typeDescriptors.length ||
            nameIndex >= strings.length ||
            typeDescriptors[classIndex] !== bridgeDescriptor
        ) {
            throw new Error("invalid JvmNative DEX method owner or name");
        }
        return strings[nameIndex];
    };

    let bridgeCount = 0;
    const nativeMethods = new Set();
    for (let index = 0; index < classDefs.size; index += 1) {
        const itemOffset = classDefs.offset + index * 32;
        const classIndex = bytes.readUInt32LE(itemOffset);
        if (classIndex >= typeDescriptors.length || typeDescriptors[classIndex] !== bridgeDescriptor) {
            continue;
        }
        bridgeCount += 1;

        const classDataOffset = bytes.readUInt32LE(itemOffset + 24);
        assertRange(bytes, classDataOffset, 1, "JvmNative class_data");
        let cursor = classDataOffset;
        const staticFields = readUleb128(bytes, cursor);
        cursor = staticFields.offset;
        const instanceFields = readUleb128(bytes, cursor);
        cursor = instanceFields.offset;
        const directMethods = readUleb128(bytes, cursor);
        cursor = directMethods.offset;
        const virtualMethods = readUleb128(bytes, cursor);
        cursor = virtualMethods.offset;

        for (let field = 0; field < staticFields.value + instanceFields.value; field += 1) {
            cursor = readUleb128(bytes, cursor).offset;
            cursor = readUleb128(bytes, cursor).offset;
        }

        const readMethods = (count) => {
            let methodIndex = 0;
            for (let method = 0; method < count; method += 1) {
                const indexDifference = readUleb128(bytes, cursor);
                cursor = indexDifference.offset;
                methodIndex += indexDifference.value;
                const accessFlags = readUleb128(bytes, cursor);
                cursor = accessFlags.offset;
                cursor = readUleb128(bytes, cursor).offset;
                if ((accessFlags.value & 0x100) !== 0) {
                    nativeMethods.add(methodName(methodIndex));
                }
            }
        };
        readMethods(directMethods.value);
        readMethods(virtualMethods.value);
    }
    return { bridgeCount, nativeMethods };
}

function readDexDirectory(directory) {
    let bridgeCount = 0;
    const nativeMethods = new Set();
    const dexPaths = fs
        .readdirSync(directory)
        .filter((name) => /^classes(?:\d+)?\.dex$/u.test(name))
        .sort()
        .map((name) => `${directory}/${name}`);
    if (dexPaths.length === 0) {
        throw new Error(`${directory} contains no classes*.dex artifact`);
    }
    for (const dexPath of dexPaths) {
        const result = readDexBridge(fs.readFileSync(dexPath), dexPath);
        bridgeCount += result.bridgeCount;
        for (const method of result.nativeMethods) {
            nativeMethods.add(method);
        }
    }
    return { bridgeCount, nativeMethods };
}

const usedDex = readDexDirectory(usedDexDirectory);
if (usedDex.bridgeCount !== 1) {
    throw new Error(`used release APK must contain exactly one ${bridgeDescriptor}; found ${usedDex.bridgeCount}`);
}
const missing = [...expectedNativeMethods].filter((method) => !usedDex.nativeMethods.has(method));
const unexpected = [...usedDex.nativeMethods].filter((method) => !expectedNativeMethods.has(method));
if (missing.length > 0 || unexpected.length > 0) {
    throw new Error(
        `used release APK JvmNative methods differ from the 13 JNI exports; ` +
            `missing=[${missing.join(", ")}], unexpected=[${unexpected.join(", ")}]`
    );
}

const unusedDex = readDexDirectory(unusedDexDirectory);
if (unusedDex.bridgeCount !== 0 || unusedDex.nativeMethods.size !== 0) {
    throw new Error(
        "unused release APK retained the JvmNative bridge even though no application path reaches the library"
    );
}

console.log(
    "Android release shrinking preserved one JvmNative class and all 13 JNI methods when used, " +
        "and removed the bridge when unused."
);
