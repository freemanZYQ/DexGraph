/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The "dexdump" tool is intended to mimic "objdump".  When possible, use
 * similar command-line arguments.
 *
 * TODO: rework the "plain" output format to be more regexp-friendly
 *
 * Differences between XML output and the "current.xml" file:
 * - classes in same package are not all grouped together; generally speaking
 *   nothing is sorted
 * - no "deprecated" on fields and methods
 * - no "value" on fields
 * - no parameter names
 * - no generic signatures on parameters, e.g. type="java.lang.Class&lt;?&gt;"
 * - class shows declared fields and methods; does not show inherited fields
 */
#include <libdex/DexFile.h>
#include <libdex/DexCatch.h>
#include <libdex/DexClass.h>
#include <libdex/DexProto.h>
#include <libdex/InstrUtils.h>
#include <libdex/SysUtil.h>
#include <libdex/CmdUtils.h>

#include <dexdump/OpCodeNames.h>

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <assert.h>
#include <string>
#include <memory>

// Modified Tool
#include <sstream>
#include <TreeConstructor/FmtEdg.h>
#include <TreeConstructor/FmtDot.h>
#include <TreeConstructor/TCHelper.h>
#include <TreeConstructor/TCNode.h>

static const char* gProgName = "dexdump";

static InstructionWidth* gInstrWidth;
static InstructionFormat* gInstrFormat;

typedef std::pair<TreeConstructor::MethodInfo, TreeConstructor::NodeSPtr>
    id_node_pair;

typedef enum OutputFormat {
    OUTPUT_PLAIN = 0,               /* default */
    OUTPUT_XML,                     /* fancy */
} OutputFormat;

/* command-line options */
struct {
    bool checksumOnly;
    bool disassemble;
    bool showFileHeaders;
    bool showSectionHeaders;
    bool ignoreBadChecksum;
    bool dumpRegisterMaps;
    OutputFormat outputFormat;
    const char* tempFileName;
    bool exportsOnly;
    bool verbose;
} gOptions;

/* basic info about a field or method */
typedef struct FieldMethodInfo {
    const char* classDescriptor;
    const char* name;
    const char* signature;
} FieldMethodInfo;

/*
 * Get 2 little-endian bytes. 
 */ 
static inline u2 get2LE(unsigned char const* pSrc)
{
    return pSrc[0] | (pSrc[1] << 8);
}   

/*
 * Get 4 little-endian bytes. 
 */ 
static inline u4 get4LE(unsigned char const* pSrc)
{
    return pSrc[0] | (pSrc[1] << 8) | (pSrc[2] << 16) | (pSrc[3] << 24);
}   

/*
 * Converts a single-character primitive type into its human-readable
 * equivalent.
 */
static const char* primitiveTypeLabel(char typeChar)
{
    switch (typeChar) {
    case 'B':   return "byte";
    case 'C':   return "char";
    case 'D':   return "double";
    case 'F':   return "float";
    case 'I':   return "int";
    case 'J':   return "long";
    case 'S':   return "short";
    case 'V':   return "void";
    case 'Z':   return "boolean";
    default:
                return "UNKNOWN";
    }
}

/*
 * Converts a type descriptor to human-readable "dotted" form.  For
 * example, "Ljava/lang/String;" becomes "java.lang.String", and
 * "[I" becomes "int[]".  Also converts '$' to '.', which means this
 * form can't be converted back to a descriptor.
 */
static char* descriptorToDot(const char* str)
{
    int targetLen = strlen(str);
    int offset = 0;
    int arrayDepth = 0;
    char* newStr;

    /* strip leading [s; will be added to end */
    while (targetLen > 1 && str[offset] == '[') {
        offset++;
        targetLen--;
    }
    arrayDepth = offset;

    if (targetLen == 1) {
        /* primitive type */
        str = primitiveTypeLabel(str[offset]);
        offset = 0;
        targetLen = strlen(str);
    } else {
        /* account for leading 'L' and trailing ';' */
        if (targetLen >= 2 && str[offset] == 'L' &&
            str[offset+targetLen-1] == ';')
        {
            targetLen -= 2;
            offset++;
        }
    }

    newStr = (char*)malloc(targetLen + arrayDepth * 2 +1);

    /* copy class name over */
    int i;
    for (i = 0; i < targetLen; i++) {
        char ch = str[offset + i];
        newStr[i] = (ch == '/' || ch == '$') ? '.' : ch;
    }

    /* add the appropriate number of brackets for arrays */
    while (arrayDepth-- > 0) {
        newStr[i++] = '[';
        newStr[i++] = ']';
    }
    newStr[i] = '\0';
//    assert(i == targetLen + arrayDepth * 2);

    return newStr;
}

/*
 * Converts the class name portion of a type descriptor to human-readable
 * "dotted" form.
 *
 * Returns a newly-allocated string.
 */
static char* descriptorClassToDot(const char* str)
{
    const char* lastSlash;
    char* newStr;
    char* cp;

    /* reduce to just the class name, trimming trailing ';' */
    lastSlash = strrchr(str, '/');
    if (lastSlash == nullptr)
        lastSlash = str + 1;        /* start past 'L' */
    else
        lastSlash++;                /* start past '/' */

    newStr = strdup(lastSlash);
    newStr[strlen(lastSlash)-1] = '\0';
    for (cp = newStr; *cp != '\0'; cp++) {
        if (*cp == '$')
            *cp = '.';
    }

    return newStr;
}

/*
 * Returns a quoted string representing the boolean value.
 */
static const char* quotedBool(bool val)
{
    if (val)
        return "\"true\"";
    else
        return "\"false\"";
}

static const char* quotedVisibility(u4 accessFlags)
{
    if ((accessFlags & ACC_PUBLIC) != 0)
        return "\"public\"";
    else if ((accessFlags & ACC_PROTECTED) != 0)
        return "\"protected\"";
    else if ((accessFlags & ACC_PRIVATE) != 0)
        return "\"private\"";
    else
        return "\"package\"";
}

/*
 * Count the number of '1' bits in a word.
 */
static int countOnes(u4 val)
{
    int count = 0;

    val = val - ((val >> 1) & 0x55555555);
    val = (val & 0x33333333) + ((val >> 2) & 0x33333333);
    count = (((val + (val >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;

    return count;
}

/*
 * Flag for use with createAccessFlagStr().
 */
typedef enum AccessFor {
    kAccessForClass = 0, kAccessForMethod = 1, kAccessForField = 2,
    kAccessForMAX
} AccessFor;

/*
 * Create a new string with human-readable access flags.
 *
 * In the base language the access_flags fields are type u2; in Dalvik
 * they're u4.
 */
static char* createAccessFlagStr(u4 flags, AccessFor forWhat)
{
#define NUM_FLAGS   18
    static const char* kAccessStrings[kAccessForMAX][NUM_FLAGS] = {
        {   
            /* class, inner class */
            "PUBLIC",           /* 0x0001 */
            "PRIVATE",          /* 0x0002 */
            "PROTECTED",        /* 0x0004 */
            "STATIC",           /* 0x0008 */
            "FINAL",            /* 0x0010 */
            "?",                /* 0x0020 */
            "?",                /* 0x0040 */
            "?",                /* 0x0080 */
            "?",                /* 0x0100 */
            "INTERFACE",        /* 0x0200 */
            "ABSTRACT",         /* 0x0400 */
            "?",                /* 0x0800 */
            "SYNTHETIC",        /* 0x1000 */
            "ANNOTATION",       /* 0x2000 */
            "ENUM",             /* 0x4000 */
            "?",                /* 0x8000 */
            "VERIFIED",         /* 0x10000 */
            "OPTIMIZED",        /* 0x20000 */
        },
        {
            /* method */
            "PUBLIC",           /* 0x0001 */
            "PRIVATE",          /* 0x0002 */
            "PROTECTED",        /* 0x0004 */
            "STATIC",           /* 0x0008 */
            "FINAL",            /* 0x0010 */
            "SYNCHRONIZED",     /* 0x0020 */
            "BRIDGE",           /* 0x0040 */
            "VARARGS",          /* 0x0080 */
            "NATIVE",           /* 0x0100 */
            "?",                /* 0x0200 */
            "ABSTRACT",         /* 0x0400 */
            "STRICT",           /* 0x0800 */
            "SYNTHETIC",        /* 0x1000 */
            "?",                /* 0x2000 */
            "?",                /* 0x4000 */
            "MIRANDA",          /* 0x8000 */
            "CONSTRUCTOR",      /* 0x10000 */
            "DECLARED_SYNCHRONIZED", /* 0x20000 */
        },
        {
            /* field */
            "PUBLIC",           /* 0x0001 */
            "PRIVATE",          /* 0x0002 */
            "PROTECTED",        /* 0x0004 */
            "STATIC",           /* 0x0008 */
            "FINAL",            /* 0x0010 */
            "?",                /* 0x0020 */
            "VOLATILE",         /* 0x0040 */
            "TRANSIENT",        /* 0x0080 */
            "?",                /* 0x0100 */
            "?",                /* 0x0200 */
            "?",                /* 0x0400 */
            "?",                /* 0x0800 */
            "SYNTHETIC",        /* 0x1000 */
            "?",                /* 0x2000 */
            "ENUM",             /* 0x4000 */
            "?",                /* 0x8000 */
            "?",                /* 0x10000 */
            "?",                /* 0x20000 */
        },
    };
    const int kLongest = 21;        /* strlen of longest string above */
    int i, count;
    char* str;
    char* cp;

    /*
     * Allocate enough storage to hold the expected number of strings,
     * plus a space between each.  We over-allocate, using the longest
     * string above as the base metric.
     */
    count = countOnes(flags);
    cp = str = (char*) malloc(count * (kLongest+1) +1);

    for (i = 0; i < NUM_FLAGS; i++) {
        if (flags & 0x01) {
            const char* accessStr = kAccessStrings[forWhat][i];
            int len = strlen(accessStr);
            if (cp != str)
                *cp++ = ' ';

            memcpy(cp, accessStr, len);
            cp += len;
        }
        flags >>= 1;
    }
    *cp = '\0';

    return str;
}


/*
 * Dump the file header.
 */
void dumpFileHeader(const DexFile* pDexFile)
{
    const DexHeader* pHeader = pDexFile->pHeader;
}

/*
 * Dump a class_def_item.
 */
void dumpClassDef(DexFile* pDexFile, int idx)
{
    const DexClassDef* pClassDef;
    const u1* pEncodedData;
    DexClassData* pClassData;

    pClassDef = dexGetClassDef(pDexFile, idx);
    pEncodedData = dexGetClassData(pDexFile, pClassDef);
    pClassData = dexReadAndVerifyClassData(&pEncodedData, nullptr);

    if (pClassData == nullptr) {
        fprintf(stderr, "Trouble reading class data\n");
        return;
    }
    free(pClassData);
}

/*
 * Dump an interface that a class declares to implement.
 */
void dumpInterface(const DexFile* pDexFile, const DexTypeItem* pTypeItem,
    int i)
{
    const char* interfaceName =
        dexStringByTypeIdx(pDexFile, pTypeItem->typeIdx);
}

/*
 * Dump the catches table associated with the code.
 */
void dumpCatches(DexFile* pDexFile, const DexCode* pCode)
{
    u4 triesSize = pCode->triesSize;

    const DexTry* pTries = dexGetTries(pCode);
    u4 i;

    for (i = 0; i < triesSize; i++) {
        const DexTry* pTry = &pTries[i];
        u4 start = pTry->startAddr;
        u4 end = start + pTry->insnCount;
        DexCatchIterator iterator;

        dexCatchIteratorInit(&iterator, pCode, pTry->handlerOff);

        for (;;) {
            DexCatchHandler* handler = dexCatchIteratorNext(&iterator);
            const char* descriptor;
            
            if (handler == nullptr) {
                break;
            }
            
            descriptor = (handler->typeIdx == kDexNoIndex) ? "<any>" : 
                dexStringByTypeIdx(pDexFile, handler->typeIdx);
        }
    }
}

static int dumpPositionsCb(void *cnxt, u4 address, u4 lineNum)
{
    return 0;
}

/*
 * Dump the positions list.
 */
void dumpPositions(DexFile* pDexFile, const DexCode* pCode, 
        const DexMethod *pDexMethod)
{
    const DexMethodId *pMethodId 
            = dexGetMethodId(pDexFile, pDexMethod->methodIdx);
    const char *classDescriptor
            = dexStringByTypeIdx(pDexFile, pMethodId->classIdx);

    dexDecodeDebugInfo(pDexFile, pCode, classDescriptor, pMethodId->protoIdx,
            pDexMethod->accessFlags, dumpPositionsCb, nullptr, nullptr);
}

static void dumpLocalsCb(void *cnxt, u2 reg, u4 startAddress,
        u4 endAddress, const char *name, const char *descriptor,
        const char *signature)
{
}

/*
 * Dump the locals list.
 */
void dumpLocals(DexFile* pDexFile, const DexCode* pCode,
        const DexMethod *pDexMethod)
{
    const DexMethodId *pMethodId 
            = dexGetMethodId(pDexFile, pDexMethod->methodIdx);
    const char *classDescriptor 
            = dexStringByTypeIdx(pDexFile, pMethodId->classIdx);

    dexDecodeDebugInfo(pDexFile, pCode, classDescriptor, pMethodId->protoIdx,
            pDexMethod->accessFlags, nullptr, dumpLocalsCb, nullptr);
}

/*
 * Get information about a method.
 */
bool getMethodInfo(DexFile* pDexFile, u4 methodIdx, FieldMethodInfo* pMethInfo)
{
    const DexMethodId* pMethodId;

    if (methodIdx >= pDexFile->pHeader->methodIdsSize)
        return false;

    pMethodId = dexGetMethodId(pDexFile, methodIdx);
    pMethInfo->name = dexStringById(pDexFile, pMethodId->nameIdx);
    pMethInfo->signature = dexCopyDescriptorFromMethodId(pDexFile, pMethodId);

    pMethInfo->classDescriptor = 
            dexStringByTypeIdx(pDexFile, pMethodId->classIdx);
    return true;
}

/*
 * Get information about a field.
 */
bool getFieldInfo(DexFile* pDexFile, u4 fieldIdx, FieldMethodInfo* pFieldInfo)
{
    const DexFieldId* pFieldId;

    if (fieldIdx >= pDexFile->pHeader->fieldIdsSize)
        return false;

    pFieldId = dexGetFieldId(pDexFile, fieldIdx);
    pFieldInfo->name = dexStringById(pDexFile, pFieldId->nameIdx);
    pFieldInfo->signature = dexStringByTypeIdx(pDexFile, pFieldId->typeIdx);
    pFieldInfo->classDescriptor =
        dexStringByTypeIdx(pDexFile, pFieldId->classIdx);
    return true;
}


/*
 * Look up a class' descriptor.
 */
const char* getClassDescriptor(DexFile* pDexFile, u4 classIdx)
{
    return dexStringByTypeIdx(pDexFile, classIdx);
}

/*
 * Dump a single instruction.
 */
TreeConstructor::Node dumpInstruction(DexFile* pDexFile, 
	const DexCode* pCode,
	int insnIdx,
  int insnWidth,
	const DecodedInstruction* pDecInsn)
{
	// Modified Tool
  std::vector<uint32_t> arg_offset;
  uint32_t payload_offset = 0;
	TreeConstructor::MethodInfo opt_called_method_info;

  u2 const* insns = pCode->insns;

  switch (dexGetInstrFormat(gInstrFormat, pDecInsn->opCode)) 
	{
    case kFmt10t: case kFmt20t:    // op+AAcase  // op +AAAA
    {
      s4 targ = (s4)pDecInsn->vA;
      arg_offset.push_back(insnIdx + targ);
      break;
    }
    case kFmt21t: // op vAA, +BBBB
    {
      s4 targ = (s4)pDecInsn->vB;
      arg_offset.push_back(insnIdx + targ);
      break;
    }
    case kFmt22t: // op vA, vB, +CCCC
    {
      s4 targ = (s4)pDecInsn->vC;
      arg_offset.push_back(insnIdx + targ);
      break;
    }
    case kFmt22s: // op vA, vB, #+CCCC
    {
      arg_offset.push_back((s4)pDecInsn->vC);
      break;
    }
    case kFmt22cs: // [opt] op vA, vB, field offset CCCC
    {
      arg_offset.push_back(pDecInsn->vC);
      break;
    }
    case kFmt31t: // op vAA, offset +BBBBBBBB
    {
      payload_offset = insnIdx + pDecInsn->vB;
      break;
    }
    case kFmt35c: // op vB, {vD, vE, vF, vG, vA}, thing@CCCC
    {
      if (pDecInsn->opCode != OP_FILLED_NEW_ARRAY) {
				try 
				{
					auto const method_idx = pDecInsn->vB;
					opt_called_method_info =
            TreeConstructor::get_method_info(*pDexFile, method_idx);
				}
				catch (std::range_error const& e) {}
      }
      break;
    }
    case kFmt3rc: // op {vCCCC .. v(CCCC+AA-1)}, meth@BBBB
    {
      if (pDecInsn->opCode != OP_FILLED_NEW_ARRAY_RANGE) {
				try
				{
					auto const method_idx = pDecInsn->vB;
          opt_called_method_info =
            TreeConstructor::get_method_info(*pDexFile, method_idx);
				}
				catch(std::range_error const& e) {}
      }
      break;
    } 
    default: break;
	}
   
	// Get relevant addresses
	intptr_t const method_base_addr = ((u1*)insns - pDexFile->baseAddr) + insnIdx * 2;
	// Get Instruction size
	auto const instr_size = insnWidth;
	// Get OpCode
	auto const instr_opcode = pDecInsn->opCode;
	// Get Offset (relative to method base address)
	int const internal_offset = insnIdx;

	/// Additional processing for Switch instructions
	if (instr_opcode == OpCode::OP_PACKED_SWITCH)
	{
		intptr_t const payload_addr = (intptr_t const)(insns + payload_offset);
		auto const packed_switch_payload =
				TreeConstructor::get_packed_switch_payload(internal_offset,
																									 payload_addr);
		for (auto const& offset : packed_switch_payload.targets)
			arg_offset.push_back(offset);
	}

	if (instr_opcode == OpCode::OP_SPARSE_SWITCH)
	{
		auto const payload_addr = (intptr_t const)(insns + payload_offset);
		auto const sparse_switch_payload =
				TreeConstructor::get_sparse_switch_offsets(internal_offset,
																									 payload_addr);
		for (auto const& offset : sparse_switch_payload.targets)  
			arg_offset.push_back(offset);
	}

	// Construct Tree Node
	auto method_node =
			TreeConstructor::Node(method_base_addr,
														instr_size,
														instr_opcode,
														opt_called_method_info,
														internal_offset,
														arg_offset);

	return method_node;
}

/*
 * Dump a bytecode disassembly.
 */
std::pair<id_node_pair, std::vector<TreeConstructor::NodeSPtr>>
dumpBytecodes(DexFile *pDexFile, const DexMethod *pDexMethod) 
{
  const DexCode* pCode = dexGetCode(pDexFile, pDexMethod);
  const u2* insns;
  int insnIdx;
  FieldMethodInfo methInfo;
  int startAddr;
  char* className = nullptr;

  assert(pCode->insnsSize > 0);
  insns = pCode->insns;

  getMethodInfo(pDexFile, pDexMethod->methodIdx, &methInfo);
  startAddr = ((u1*)pCode - pDexFile->baseAddr);

  className = descriptorToDot(methInfo.classDescriptor);

  insnIdx = 0;
  std::vector<TreeConstructor::NodeSPtr> node_vector;
  while (insnIdx < (int) pCode->insnsSize) {
    int insnWidth;
    OpCode opCode;
    DecodedInstruction decInsn;
    u2 instr;

    instr = get2LE((const u1*)insns);
    if (instr == kPackedSwitchSignature) {
      insnWidth = 4 + get2LE((const u1*)(insns+1)) * 2;
    } else if (instr == kSparseSwitchSignature) {
      insnWidth = 2 + get2LE((const u1*)(insns+1)) * 4;
    } else if (instr == kArrayDataSignature) {
      int width = get2LE((const u1*)(insns+1));
      int size = get2LE((const u1*)(insns+2)) | 
                  (get2LE((const u1*)(insns+3))<<16);
      // The plus 1 is to round up for odd size and width 
      insnWidth = 4 + ((size * width) + 1) / 2;
    } else {
      opCode = (OpCode)(instr & 0xff);
      insnWidth = dexGetInstrWidthAbs(gInstrWidth, opCode);
      if (insnWidth == 0) {
        fprintf(stderr,
            "GLITCH: zero-width instruction at idx=0x%04x\n", insnIdx);
        break;
      }
    }

    dexDecodeInstruction(gInstrFormat, insns, &decInsn);

    auto const instr_node =
        dumpInstruction(pDexFile, pCode, insnIdx, insnWidth, &decInsn);

    insns += insnWidth;
    insnIdx += insnWidth;

    // Add to node vector
    node_vector.push_back(std::make_shared<TreeConstructor::Node>(instr_node));
  }

  auto const method_info =
      TreeConstructor::get_method_info(*pDexFile, pDexMethod->methodIdx);
	auto const call_nodes = TreeConstructor::get_method_call_nodes(node_vector);
  auto const nodeptr = TreeConstructor::construct_node_from_vec(node_vector);
  free(className);

	auto const methodid_node_pair = std::make_pair(method_info, nodeptr);
	return std::make_pair(methodid_node_pair, call_nodes);
}

/*
 * Dump a "code" struct.
 */
std::pair<id_node_pair, std::vector<TreeConstructor::NodeSPtr>>
dumpCode(DexFile *pDexFile, const DexMethod *pDexMethod) 
{
  const DexCode *pCode = dexGetCode(pDexFile, pDexMethod);

  if (gOptions.disassemble)
    return dumpBytecodes(pDexFile, pDexMethod);
  else
    throw std::runtime_error("Could not dump byte_code for method_id " +
                             std::to_string(pDexMethod->methodIdx));
}

/*
 * Dump a method.
 */
std::pair<id_node_pair, std::vector<TreeConstructor::NodeSPtr>>
dumpMethod(DexFile *pDexFile, const DexMethod *pDexMethod, int i) 
{
  if (gOptions.exportsOnly &&
      (pDexMethod->accessFlags & (ACC_PUBLIC | ACC_PROTECTED)) == 0) {
    throw std::runtime_error("Could not access method with method_idx: " +
                             std::to_string(pDexMethod->methodIdx));
  }

  if (pDexMethod->codeOff != 0)
    return dumpCode(pDexFile, pDexMethod);
  else
    throw std::runtime_error("codeOff for method_idx " +
                             std::to_string(pDexMethod->methodIdx) +
                             " equals 0.");
}

/*
 * Dump a static (class) field.
 */
void dumpSField(const DexFile* pDexFile, const DexField* pSField, int i)
{
    const DexFieldId* pFieldId;
    const char* backDescriptor;
    const char* name;
    const char* typeDescriptor;

    if (gOptions.exportsOnly &&
        (pSField->accessFlags & (ACC_PUBLIC | ACC_PROTECTED)) == 0)
    {
        return;
    }

    pFieldId = dexGetFieldId(pDexFile, pSField->fieldIdx);
    name = dexStringById(pDexFile, pFieldId->nameIdx);
    typeDescriptor = dexStringByTypeIdx(pDexFile, pFieldId->typeIdx);
    backDescriptor = dexStringByTypeIdx(pDexFile, pFieldId->classIdx);

}

/*
 * Dump an instance field.
 */
void dumpIField(const DexFile* pDexFile, const DexField* pIField, int i)
{
    dumpSField(pDexFile, pIField, i);
}

/*
 * Dump the class.
 *
 * Note "idx" is a DexClassDef index, not a DexTypeId index.
 *
 * If "*pLastPackage" is nullptr or does not match the current class' package,
 * the value will be replaced with a newly-allocated string.
 */
std::pair<std::map<TreeConstructor::MethodInfo, TreeConstructor::NodeSPtr>,
          std::vector<TreeConstructor::NodeSPtr>>
dumpClass(
		DexFile *pDexFile,
	 	int idx,
	 	char **pLastPackage) 
{
  const DexTypeList *pInterfaces;
  const DexClassDef *pClassDef;
  DexClassData *pClassData = nullptr;
  const u1 *pEncodedData;
  const char *fileName;
  const char *classDescriptor;
  const char *superclassDescriptor;
  char *accessStr = nullptr;
  int i;

  std::map<TreeConstructor::MethodInfo, TreeConstructor::NodeSPtr> ret;
	std::vector<TreeConstructor::NodeSPtr> call_node_vec;

  pClassDef = dexGetClassDef(pDexFile, idx);

  pEncodedData = dexGetClassData(pDexFile, pClassDef);
  pClassData = dexReadAndVerifyClassData(&pEncodedData, nullptr);

  if (pClassData == nullptr) {
    free(pClassData);
    free(accessStr);
    return std::make_pair(ret, call_node_vec);
  }

  classDescriptor = dexStringByTypeIdx(pDexFile, pClassDef->classIdx);

  /*
   * For the XML output, show the package name.  Ideally we'd gather
   * up the classes, sort them, and dump them alphabetically so the
   * package name wouldn't jump around, but that's not a great plan
   * for something that needs to run on the device.
   */
  if (!(classDescriptor[0] == 'L' &&
        classDescriptor[strlen(classDescriptor) - 1] == ';')) {
    /* arrays and primitives should not be defined explicitly */
    fprintf(stderr, "Malformed class name '%s'\n", classDescriptor);
    /* keep going? */
  } else if (gOptions.outputFormat == OUTPUT_XML) {
    char *mangle;
    char *lastSlash;
    char *cp;

    mangle = strdup(classDescriptor + 1);
    mangle[strlen(mangle) - 1] = '\0';

    /* reduce to just the package name */
    lastSlash = strrchr(mangle, '/');
    if (lastSlash != nullptr) {
      *lastSlash = '\0';
    } else {
      *mangle = '\0';
    }

    for (cp = mangle; *cp != '\0'; cp++) {
      if (*cp == '/')
        *cp = '.';
    }

    if (*pLastPackage == nullptr || strcmp(mangle, *pLastPackage) != 0) {
      /* start of a new package */
      free(*pLastPackage);
      *pLastPackage = mangle;
    } else {
      free(mangle);
    }
  }

  accessStr = createAccessFlagStr(pClassDef->accessFlags, kAccessForClass);

  if (pClassDef->superclassIdx == kDexNoIndex) {
    superclassDescriptor = nullptr;
  } else {
    superclassDescriptor =
        dexStringByTypeIdx(pDexFile, pClassDef->superclassIdx);
  }

  pInterfaces = dexGetInterfacesList(pDexFile, pClassDef);

  for (i = 0; i < (int)pClassData->header.directMethodsSize; i++) 
	{
    try 
		{
      auto const pair = dumpMethod(pDexFile, &pClassData->directMethods[i], i);
      ret.emplace(pair.first.first, pair.first.second);
      call_node_vec.insert(call_node_vec.end(), pair.second.begin(),
                           pair.second.end());
    } catch (std::runtime_error const &e) {}
  }

  for (i = 0; i < (int)pClassData->header.virtualMethodsSize; i++) 
	{
    try 
		{
      auto const pair = dumpMethod(pDexFile, &pClassData->virtualMethods[i], i);
      ret.emplace(pair.first.first, pair.first.second);
      call_node_vec.insert(call_node_vec.end(), pair.second.begin(),
                           pair.second.end());
    } catch (std::runtime_error const &e) {}
  }

  // TODO: Annotations.

  if (pClassDef->sourceFileIdx != kDexNoIndex)
    fileName = dexStringById(pDexFile, pClassDef->sourceFileIdx);
  else
    fileName = "unknown";

  return std::make_pair(ret, call_node_vec);
}


/*
 * Advance "ptr" to ensure 32-bit alignment.
 */
static inline const u1* align32(const u1* ptr)
{
    return (u1*) (((intptr_t) ptr + 3) & ~0x03);
}


/*
 * Dump a map in the "differential" format.
 *
 * TODO: show a hex dump of the compressed data.  (We can show the
 * uncompressed data if we move the compression code to libdex; otherwise
 * it's too complex to merit a fast & fragile implementation here.)
 */
void dumpDifferentialCompressedMap(const u1** pData)
{
    const u1* data = *pData;
    const u1* dataStart = data -1;      // format byte already removed
    u1 regWidth;
    u2 numEntries;

    /* standard header */
    regWidth = *data++;
    numEntries = *data++;
    numEntries |= (*data++) << 8;

    /* compressed data begins with the compressed data length */
    int compressedLen = readUnsignedLeb128(&data);
    int addrWidth = 1;
    if ((*data & 0x80) != 0)
        addrWidth++;

    int origLen = 4 + (addrWidth + regWidth) * numEntries;
    int compLen = (data - dataStart) + compressedLen;

    /* skip past end of entry */
    data += compressedLen;

    *pData = data;
}

/*
 * Dump register map contents of the current method.
 *
 * "*pData" should point to the start of the register map data.  Advances
 * "*pData" to the start of the next map.
 */
void dumpMethodMap(DexFile* pDexFile, const DexMethod* pDexMethod, int idx,
    const u1** pData)
{
    const u1* data = *pData;
    const DexMethodId* pMethodId;
    const char* name;
    int offset = data - (u1*) pDexFile->pOptHeader;

    pMethodId = dexGetMethodId(pDexFile, pDexMethod->methodIdx);
    name = dexStringById(pDexFile, pMethodId->nameIdx);

    u1 format;
    int addrWidth;

    format = *data++;
    if (format == 1) {              /* kRegMapFormatNone */
        /* no map */
        addrWidth = 0;
    } else if (format == 2) {       /* kRegMapFormatCompact8 */
        addrWidth = 1;
    } else if (format == 3) {       /* kRegMapFormatCompact16 */
        addrWidth = 2;
    } else if (format == 4) {       /* kRegMapFormatDifferential */
        dumpDifferentialCompressedMap(&data);
        goto bail;
    } else {
        /* don't know how to skip data; failure will cascade to end of class */
        goto bail;
    }

    if (addrWidth > 0) 
		{
      u1 regWidth;
      u2 numEntries;
      int idx, addr, byte;

      regWidth = *data++;
      numEntries = *data++;
      numEntries |= (*data++) << 8;

      for (idx = 0; idx < numEntries; idx++) 
			{
        addr = *data++;
        if (addrWidth > 1)
          addr |= (*data++) << 8;
      }
    }

bail:
    //if (addrWidth >= 0)
    //    *pData = align32(data);
    *pData = data;
}

/*
 * Dump the contents of the register map area.
 *
 * These are only present in optimized DEX files, and the structure is
 * not really exposed to other parts of the VM itself.  We're going to
 * dig through them here, but this is pretty fragile.  DO NOT rely on
 * this or derive other code from it.
 */
void dumpRegisterMaps(DexFile* pDexFile)
{
    const u1* pClassPool = (const u1*)pDexFile->pRegisterMapPool;
    const u4* classOffsets;
    const u1* ptr;
    u4 numClasses;
    int baseFileOffset = (u1*) pClassPool - (u1*) pDexFile->pOptHeader;
    int idx;

    if (pClassPool == nullptr) {
        return;
    }

    ptr = pClassPool;
    numClasses = get4LE(ptr);
    ptr += sizeof(u4);
    classOffsets = (const u4*) ptr;

    for (idx = 0; idx < (int) numClasses; idx++) {
        const DexClassDef* pClassDef;
        const char* classDescriptor;

        pClassDef = dexGetClassDef(pDexFile, idx);
        classDescriptor = dexStringByTypeIdx(pDexFile, pClassDef->classIdx);

        if (classOffsets[idx] == 0)
            continue;

        /*
         * What follows is a series of RegisterMap entries, one for every
         * direct method, then one for every virtual method.
         */
        DexClassData* pClassData;
        const u1* pEncodedData;
        const u1* data = (u1*) pClassPool + classOffsets[idx];
        u2 methodCount;
        int i;

        pEncodedData = dexGetClassData(pDexFile, pClassDef);
        pClassData = dexReadAndVerifyClassData(&pEncodedData, nullptr);
        if (pClassData == nullptr) {
            fprintf(stderr, "Trouble reading class data\n");
            continue;
        }

        methodCount = *data++;
        methodCount |= (*data++) << 8;
        data += 2;      /* two pad bytes follow methodCount */

        for (i = 0; i < (int) pClassData->header.directMethodsSize; i++) {
            dumpMethodMap(pDexFile, &pClassData->directMethods[i], i, &data);
        }

        for (i = 0; i < (int) pClassData->header.virtualMethodsSize; i++) {
            dumpMethodMap(pDexFile, &pClassData->virtualMethods[i], i, &data);
        }

        free(pClassData);
    }
}

/*
 * Dump the requested sections of the file.
 */
void processDexFile(const char *fileName, DexFile *pDexFile)
{
  char *package = nullptr;
  int i;

  if (gOptions.dumpRegisterMaps) {
    dumpRegisterMaps(pDexFile);
    return;
  }

  if (gOptions.showFileHeaders)
    dumpFileHeader(pDexFile);

	// Construct {method, node} map for each method in the program.
  std::map<TreeConstructor::MethodInfo, TreeConstructor::NodeSPtr>
      method_node_map;
  std::vector<TreeConstructor::NodeSPtr> call_node_vec;
  for (i = 0; i < (int)pDexFile->pHeader->classDefsSize; i++)
  {
    if (gOptions.showSectionHeaders)
      dumpClassDef(pDexFile, i);

    auto const pair = dumpClass(pDexFile, i, &package);
    auto const class_map = pair.first;
    auto const node_vec = pair.second;
    
    method_node_map.insert(class_map.begin(), class_map.end());
    call_node_vec.insert(call_node_vec.end(), node_vec.begin(), node_vec.end());
  }

  // Now that we have all the methods, we can resolve CALL instructions.
  TreeConstructor::process_calls(method_node_map, call_node_vec);
	
	// Dump result using given format
  //for (auto const& pair : method_node_map)
    //Fmt::Dot::dump_tree(*(pair.second));
  
  std::vector<TreeConstructor::NodeSPtr> nodesptr_vec;
  std::vector<std::pair<TreeConstructor::NodeSPtr,
                        TreeConstructor::NodeSPtr>> edges_vec;
  for (auto const& pair: method_node_map)
  {
    std::vector<TreeConstructor::NodeSPtr> current_nodesptr_vec;
    std::vector<std::pair<TreeConstructor::NodeSPtr,
                          TreeConstructor::NodeSPtr>> current_edges_vec;
    std::tie(current_nodesptr_vec, current_edges_vec) =
      TreeConstructor::binary_traversal(*pair.second,
                                        Fmt::Edg::dump_single_node);
    // Update global vecs
		for (auto const& node : current_nodesptr_vec)
		{
			auto const it = std::find_if(nodesptr_vec.begin(),
                                   nodesptr_vec.end(),
                                   [&](TreeConstructor::NodeSPtr current_node) -> bool {
                                     return current_node->baseAddr == node->baseAddr;
                                   });
			if (it == std::end(nodesptr_vec))
				nodesptr_vec.push_back(node);
		}
															
    edges_vec.insert(edges_vec.begin(),
                     current_edges_vec.begin(),
                     current_edges_vec.end());
  }
  // Dump all in Edg format
  Fmt::Edg::dump_all(nodesptr_vec, edges_vec);
  

  /* free the last one allocated */
  if (package != nullptr) {
    free(package);
  }
}


/*
 * Process one file.
 */
int process(const char* fileName)
{
    DexFile* pDexFile = nullptr;
    MemMapping map;
    bool mapped = false;
    int result = -1;

    int flags = kDexParseVerifyChecksum;

    if (dexOpenAndMap(fileName, gOptions.tempFileName, &map, false) != 0)
        goto bail;
    mapped = true;

    if (gOptions.ignoreBadChecksum)
        flags |= kDexParseContinueOnError;

	pDexFile = dexFileParse((const u1*)map.addr, map.length, flags);
    if (pDexFile == nullptr) {
        fprintf(stderr, "ERROR: DEX parse failed\n");
        goto bail;
    }

    if (gOptions.checksumOnly) {
    } else {
        processDexFile(fileName, pDexFile);
    }

    result = 0;

bail:
    if (mapped)
        sysReleaseShmem(&map);
    if (pDexFile != nullptr)
        dexFileFree(pDexFile);
    return result;
}


/*
 * Show usage.
 */
void usage(void)
{
    fprintf(stderr, "Copyright (C) 2007 The Android Open Source Project\n\n");
    fprintf(stderr,
        "%s: [-c] [-d] [-f] [-h] [-i] [-l layout] [-m] [-t tempfile] dexfile...\n",
        gProgName);
    fprintf(stderr, "\n");
    fprintf(stderr, " -c : verify checksum and exit\n");
    fprintf(stderr, " -d : disassemble code sections\n");
    fprintf(stderr, " -f : display summary information from file header\n");
    fprintf(stderr, " -h : display file header details\n");
    fprintf(stderr, " -i : ignore checksum failures\n");
    fprintf(stderr, " -l : output layout, either 'plain' or 'xml'\n");
    fprintf(stderr, " -m : dump register maps (and nothing else)\n");
    fprintf(stderr, " -t : temp file name (defaults to /sdcard/dex-temp-*)\n");
}

/*
 * Parse args.
 *
 * I'm not using getopt_long() because we may not have it in libc.
 */
int main(int argc, char* const argv[])
{
    bool wantUsage = false;
    int ic;

    memset(&gOptions, 0, sizeof(gOptions));
    gOptions.verbose = true;

    while (1) {
        ic = getopt(argc, argv, "cdfhil:mt:");
        if (ic < 0)
            break;

        switch (ic) {
        case 'c':       // verify the checksum then exit
            gOptions.checksumOnly = true;
            break;
        case 'd':       // disassemble Dalvik instructions
            gOptions.disassemble = true;
            break;
        case 'f':       // dump outer file header
            gOptions.showFileHeaders = true;
            break;
        case 'h':       // dump section headers, i.e. all meta-data
            gOptions.showSectionHeaders = true;
            break;
        case 'i':       // continue even if checksum is bad
            gOptions.ignoreBadChecksum = true;
            break;
        case 'l':       // layout
            if (strcmp(optarg, "plain") == 0) {
                gOptions.outputFormat = OUTPUT_PLAIN;
            } else if (strcmp(optarg, "xml") == 0) {
                gOptions.outputFormat = OUTPUT_XML;
                gOptions.verbose = false;
                gOptions.exportsOnly = true;
            } else {
                wantUsage = true;
            }
            break;
        case 'm':       // dump register maps only
            gOptions.dumpRegisterMaps = true;
            break;
        case 't':       // temp file, used when opening compressed Jar
            gOptions.tempFileName = optarg;
            break;
        default:
            wantUsage = true;
            break;
        }
    }

    if (optind == argc) {
        fprintf(stderr, "%s: no file specified\n", gProgName);
        wantUsage = true;
    }

    if (gOptions.checksumOnly && gOptions.ignoreBadChecksum) {
        fprintf(stderr, "Can't specify both -c and -i\n");
        wantUsage = true;
    }

    /* initialize some VM tables */
    gInstrWidth = dexCreateInstrWidthTable();
    gInstrFormat = dexCreateInstrFormatTable();

    if (wantUsage) {
        usage();
        return 2;
    }

    int result = 0;
    while (optind < argc) {
        result |= process(argv[optind++]);
    }

    free(gInstrWidth);
    free(gInstrFormat);

    return (result != 0);
}

