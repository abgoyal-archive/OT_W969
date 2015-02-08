#ifndef EXPAT_INTERFACE_DEF_H
#define EXPAT_INTERFACE_DEF_H


#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif
#ifndef EXEC_EXEC_H
#include <exec/exec.h>
#endif
#ifndef EXEC_INTERFACES_H
#include <exec/interfaces.h>
#endif

#ifndef LIBRARIES_EXPAT_H
#include <libraries/expat.h>
#endif

struct ExpatIFace
{
	struct InterfaceData Data;

	uint32 APICALL (*Obtain)(struct ExpatIFace *Self);
	uint32 APICALL (*Release)(struct ExpatIFace *Self);
	void APICALL (*Expunge)(struct ExpatIFace *Self);
	struct Interface * APICALL (*Clone)(struct ExpatIFace *Self);
	XML_Parser APICALL (*XML_ParserCreate)(struct ExpatIFace *Self, const XML_Char * encodingName);
	XML_Parser APICALL (*XML_ParserCreateNS)(struct ExpatIFace *Self, const XML_Char * encodingName, XML_Char nsSep);
	XML_Parser APICALL (*XML_ParserCreate_MM)(struct ExpatIFace *Self, const XML_Char * encoding, const XML_Memory_Handling_Suite * memsuite, const XML_Char * namespaceSeparator);
	XML_Parser APICALL (*XML_ExternalEntityParserCreate)(struct ExpatIFace *Self, XML_Parser parser, const XML_Char * context, const XML_Char * encoding);
	void APICALL (*XML_ParserFree)(struct ExpatIFace *Self, XML_Parser parser);
	enum XML_Status APICALL (*XML_Parse)(struct ExpatIFace *Self, XML_Parser parser, const char * s, int len, int isFinal);
	enum XML_Status APICALL (*XML_ParseBuffer)(struct ExpatIFace *Self, XML_Parser parser, int len, int isFinal);
	void * APICALL (*XML_GetBuffer)(struct ExpatIFace *Self, XML_Parser parser, int len);
	void APICALL (*XML_SetStartElementHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartElementHandler start);
	void APICALL (*XML_SetEndElementHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_EndElementHandler end);
	void APICALL (*XML_SetElementHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartElementHandler start, XML_EndElementHandler end);
	void APICALL (*XML_SetCharacterDataHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_CharacterDataHandler handler);
	void APICALL (*XML_SetProcessingInstructionHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_ProcessingInstructionHandler handler);
	void APICALL (*XML_SetCommentHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_CommentHandler handler);
	void APICALL (*XML_SetStartCdataSectionHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartCdataSectionHandler start);
	void APICALL (*XML_SetEndCdataSectionHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_EndCdataSectionHandler end);
	void APICALL (*XML_SetCdataSectionHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartCdataSectionHandler start, XML_EndCdataSectionHandler end);
	void APICALL (*XML_SetDefaultHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_DefaultHandler handler);
	void APICALL (*XML_SetDefaultHandlerExpand)(struct ExpatIFace *Self, XML_Parser parser, XML_DefaultHandler handler);
	void APICALL (*XML_SetExternalEntityRefHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_ExternalEntityRefHandler handler);
	void APICALL (*XML_SetExternalEntityRefHandlerArg)(struct ExpatIFace *Self, XML_Parser parser, void * arg);
	void APICALL (*XML_SetUnknownEncodingHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_UnknownEncodingHandler handler, void * data);
	void APICALL (*XML_SetStartNamespaceDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartNamespaceDeclHandler start);
	void APICALL (*XML_SetEndNamespaceDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_EndNamespaceDeclHandler end);
	void APICALL (*XML_SetNamespaceDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartNamespaceDeclHandler start, XML_EndNamespaceDeclHandler end);
	void APICALL (*XML_SetXmlDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_XmlDeclHandler handler);
	void APICALL (*XML_SetStartDoctypeDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartDoctypeDeclHandler start);
	void APICALL (*XML_SetEndDoctypeDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_EndDoctypeDeclHandler end);
	void APICALL (*XML_SetDoctypeDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_StartDoctypeDeclHandler start, XML_EndDoctypeDeclHandler end);
	void APICALL (*XML_SetElementDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_ElementDeclHandler eldecl);
	void APICALL (*XML_SetAttlistDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_AttlistDeclHandler attdecl);
	void APICALL (*XML_SetEntityDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_EntityDeclHandler handler);
	void APICALL (*XML_SetUnparsedEntityDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_UnparsedEntityDeclHandler handler);
	void APICALL (*XML_SetNotationDeclHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_NotationDeclHandler handler);
	void APICALL (*XML_SetNotStandaloneHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_NotStandaloneHandler handler);
	enum XML_Error APICALL (*XML_GetErrorCode)(struct ExpatIFace *Self, XML_Parser parser);
	const XML_LChar * APICALL (*XML_ErrorString)(struct ExpatIFace *Self, enum XML_Error code);
	long APICALL (*XML_GetCurrentByteIndex)(struct ExpatIFace *Self, XML_Parser parser);
	int APICALL (*XML_GetCurrentLineNumber)(struct ExpatIFace *Self, XML_Parser parser);
	int APICALL (*XML_GetCurrentColumnNumber)(struct ExpatIFace *Self, XML_Parser parser);
	int APICALL (*XML_GetCurrentByteCount)(struct ExpatIFace *Self, XML_Parser parser);
	const char * APICALL (*XML_GetInputContext)(struct ExpatIFace *Self, XML_Parser parser, int * offset, int * size);
	void APICALL (*XML_SetUserData)(struct ExpatIFace *Self, XML_Parser parser, void * userData);
	void APICALL (*XML_DefaultCurrent)(struct ExpatIFace *Self, XML_Parser parser);
	void APICALL (*XML_UseParserAsHandlerArg)(struct ExpatIFace *Self, XML_Parser parser);
	enum XML_Status APICALL (*XML_SetBase)(struct ExpatIFace *Self, XML_Parser parser, const XML_Char * base);
	const XML_Char * APICALL (*XML_GetBase)(struct ExpatIFace *Self, XML_Parser parser);
	int APICALL (*XML_GetSpecifiedAttributeCount)(struct ExpatIFace *Self, XML_Parser parser);
	int APICALL (*XML_GetIdAttributeIndex)(struct ExpatIFace *Self, XML_Parser parser);
	enum XML_Status APICALL (*XML_SetEncoding)(struct ExpatIFace *Self, XML_Parser parser, const XML_Char * encoding);
	int APICALL (*XML_SetParamEntityParsing)(struct ExpatIFace *Self, XML_Parser parser, enum XML_ParamEntityParsing parsing);
	void APICALL (*XML_SetReturnNSTriplet)(struct ExpatIFace *Self, XML_Parser parser, int do_nst);
	const XML_LChar * APICALL (*XML_ExpatVersion)(struct ExpatIFace *Self);
	XML_Expat_Version APICALL (*XML_ExpatVersionInfo)(struct ExpatIFace *Self);
	XML_Bool APICALL (*XML_ParserReset)(struct ExpatIFace *Self, XML_Parser parser, const XML_Char * encoding);
	void APICALL (*XML_SetSkippedEntityHandler)(struct ExpatIFace *Self, XML_Parser parser, XML_SkippedEntityHandler handler);
	enum XML_Error APICALL (*XML_UseForeignDTD)(struct ExpatIFace *Self, XML_Parser parser, XML_Bool useDTD);
	const XML_Feature * APICALL (*XML_GetFeatureList)(struct ExpatIFace *Self);
	enum XML_Status APICALL (*XML_StopParser)(struct ExpatIFace *Self, XML_Parser parser, XML_Bool resumable);
	enum XML_Status APICALL (*XML_ResumeParser)(struct ExpatIFace *Self, XML_Parser parser);
	void APICALL (*XML_GetParsingStatus)(struct ExpatIFace *Self, XML_Parser parser, XML_ParsingStatus * status);
	void APICALL (*XML_FreeContentModel)(struct ExpatIFace *Self, XML_Parser parser, XML_Content * model);
	void * APICALL (*XML_MemMalloc)(struct ExpatIFace *Self, XML_Parser parser, size_t size);
	void * APICALL (*XML_MemRealloc)(struct ExpatIFace *Self, XML_Parser parser, void * ptr, size_t size);
	void APICALL (*XML_MemFree)(struct ExpatIFace *Self, XML_Parser parser, void * ptr);
};

#endif /* EXPAT_INTERFACE_DEF_H */
