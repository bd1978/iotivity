//******************************************************************
//
// Copyright 2014 Intel Mobile Communications GmbH All Rights Reserved.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=


//=============================================================================
// Includes
//=============================================================================
#define _POSIX_C_SOURCE 200112L
#include <string.h>
#include "occoap.h"
#include "ocstackconfig.h"
#include "occlientcb.h"
#include "ocobserve.h"
#include "logger.h"
#include "ocmalloc.h"
#include <coap.h>

#ifndef WITH_ARDUINO
#include <unistd.h>
#endif
#include <limits.h>
#include <ctype.h>

//=============================================================================
// Macros
//=============================================================================
#define TAG    PCF("OCCoAP")
#define VERIFY_SUCCESS(op, successCode) { if (op != successCode) \
            {OC_LOG_V(FATAL, TAG, "%s failed!!", #op); goto exit;} }
#define VERIFY_NON_NULL(arg) { if (!arg) {OC_LOG_V(FATAL, TAG, "%s is NULL", #arg); goto exit;} }

//=============================================================================
// Defines
//=============================================================================
#define COAP_BLOCK_FILL_VALUE   (0xFF)

//=============================================================================
// Private Variables
//=============================================================================

static coap_context_t *gCoAPCtx = NULL;

//=============================================================================
// Helper Functions
//=============================================================================

//generate a coap token
void OCGenerateCoAPToken(OCCoAPToken * token)
{
    if (token)
    {
        token->tokenLength = MAX_TOKEN_LENGTH;
        OCFillRandomMem((uint8_t*)token->token, token->tokenLength);
    }
}

//This function is called back by libcoap when ack or rst are received
static void HandleCoAPAckRst(struct coap_context_t * ctx, uint8_t msgType,
        const coap_queue_t * sentQueue){

    // silence warnings
    (void) ctx;
    coap_pdu_t * sentPdu = sentQueue->pdu;
    OCStackResult result = OC_STACK_ERROR;
    uint32_t observationOption = OC_OBSERVE_NO_OPTION;
    // {{0}} to eliminate warning for known compiler bug 53119
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119

#ifdef CA_INT
    CAToken_t  sentToken = NULL;
#else // CA_INT
    OCCoAPToken sentToken = {{0}};
#endif // CA_INT

    result = ParseCoAPPdu(sentPdu, NULL, NULL, &observationOption, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL);
    VERIFY_SUCCESS(result, OC_STACK_OK);

    // fill OCCoAPToken structure
    RetrieveOCCoAPToken(sentPdu, &sentToken);

    if(msgType == COAP_MESSAGE_RST)
    {
        if(myStackMode != OC_CLIENT)
        {
            result = OCStackFeedBack(&sentToken, OC_OBSERVER_NOT_INTERESTED);
            if(result == OC_STACK_OK)
            {
#ifdef CA_INT
                OC_LOG_V(DEBUG, TAG,
                        "Received RST, removing all queues associated with Token %d bytes",
                        CA_MAX_TOKEN_LEN);
                OC_LOG_BUFFER(INFO, TAG, sentToken, CA_MAX_TOKEN_LEN);
                coap_cancel_all_messages(ctx, &sentQueue->remote, (unsigned char *)sentToken,
                        CA_MAX_TOKEN_LEN);
#else
                OC_LOG_V(DEBUG, TAG,
                        "Received RST, removing all queues associated with Token %d bytes",
                        sentToken.tokenLength);
                OC_LOG_BUFFER(INFO, TAG, sentToken.token, sentToken.tokenLength);
                coap_cancel_all_messages(ctx, &sentQueue->remote, sentToken.token,
                        sentToken.tokenLength);
#endif
            }
        }
    }
    else if(observationOption != OC_OBSERVE_NO_OPTION && msgType == COAP_MESSAGE_ACK)
    {
#ifdef CA_INT
#else
        OC_LOG_V(DEBUG, TAG, "Received ACK, for Token %d bytes",sentToken.tokenLength);
        OC_LOG_BUFFER(INFO, TAG, sentToken.token, sentToken.tokenLength);
#endif
        // now the observer is still interested
        if(myStackMode != OC_CLIENT)
        {
            OCStackFeedBack(&sentToken, OC_OBSERVER_STILL_INTERESTED);
        }
    }
exit:
    return;
}

//This function is called back by libcoap when a request is received
static void HandleCoAPRequests(struct coap_context_t *ctx,
        const coap_queue_t * rcvdRequest)
{
    // silence warnings
    (void) ctx;
    OCServerProtocolRequest protocolRequest = {(OCMethod)0};
    coap_block_t rcvdBlock1;
    coap_block_t rcvdBlock2;
    memset(&rcvdBlock1, COAP_BLOCK_FILL_VALUE, sizeof(coap_block_t));
    memset(&rcvdBlock2, COAP_BLOCK_FILL_VALUE, sizeof(coap_block_t));
    uint16_t rcvdSize1 = 0;
    coap_pdu_t * rcvdPdu = rcvdRequest->pdu;
    coap_pdu_t * sendPdu = NULL;
    coap_send_flags_t sendFlag;
    OCStackResult result = OC_STACK_ERROR;
    OCStackResult requestResult = OC_STACK_ERROR;

    if(myStackMode == OC_CLIENT)
    {
        //TODO: should the client be responding to requests?
        return;
    }

    protocolRequest.observationOption = OC_OBSERVE_NO_OPTION;
    protocolRequest.qos = (rcvdPdu->hdr->type == COAP_MESSAGE_CON) ?
            OC_HIGH_QOS : OC_LOW_QOS;
    protocolRequest.coapID = rcvdPdu->hdr->id;
    protocolRequest.delayedResNeeded = rcvdRequest->delayedResNeeded;
    protocolRequest.secured = rcvdRequest->secure;

    // fill OCCoAPToken structure
    RetrieveOCCoAPToken(rcvdPdu, &protocolRequest.requestToken);
#ifdef CA_INT
#else
    OC_LOG_V(INFO, TAG, " Token received %d bytes",
            protocolRequest.requestToken.tokenLength);
    OC_LOG_BUFFER(INFO, TAG, protocolRequest.requestToken.token,
            protocolRequest.requestToken.tokenLength);
#endif

    // fill OCDevAddr
    memcpy(&protocolRequest.requesterAddr, (OCDevAddr *) &rcvdRequest->remote,
            sizeof(OCDevAddr));

    // Retrieve Uri and Query from received coap pdu
    result =  ParseCoAPPdu(rcvdPdu, protocolRequest.resourceUrl,
            protocolRequest.query,
            &(protocolRequest.observationOption), NULL,
            &(protocolRequest.numRcvdVendorSpecificHeaderOptions),
            protocolRequest.rcvdVendorSpecificHeaderOptions,
            &rcvdBlock1, &rcvdBlock2, &rcvdSize1, NULL,
            protocolRequest.reqJSONPayload);
    VERIFY_SUCCESS(result, OC_STACK_OK);

    switch (rcvdPdu->hdr->code)
    {
        case COAP_REQUEST_GET:
        {
            protocolRequest.method = OC_REST_GET;
            break;
        }
        case COAP_REQUEST_POST:
        {
            protocolRequest.method = OC_REST_POST;
            break;
        }
        case COAP_REQUEST_DELETE:
        {
            protocolRequest.method = OC_REST_DELETE;
            break;
        }
        case COAP_REQUEST_PUT:
        {
            protocolRequest.method = OC_REST_PUT;
            break;
        }
        default:
        {
            OC_LOG_V(ERROR, TAG, "Received CoAP method %d not supported",
                    rcvdPdu->hdr->code);
            goto exit;
        }
    }

    if(rcvdBlock1.szx != 7)
    {
        protocolRequest.reqPacketSize = 1 << (rcvdBlock1.szx + 4);
        protocolRequest.reqMorePacket = rcvdBlock1.m;
        protocolRequest.reqPacketNum  = rcvdBlock1.num;
    }
    else
    {
        // No block1 received
        rcvdSize1= strlen((const char*)protocolRequest.reqJSONPayload)+1;
        protocolRequest.reqTotalSize = rcvdSize1;
    }

    if(rcvdBlock2.szx != 7)
    {
        protocolRequest.resPacketSize = 1 << (rcvdBlock2.szx + 4);
        protocolRequest.resPacketNum  = rcvdBlock2.num;
    }

    requestResult = HandleStackRequests(&protocolRequest);

    if(requestResult == OC_STACK_VIRTUAL_DO_NOT_HANDLE ||
            requestResult == OC_STACK_OK ||
            requestResult == OC_STACK_RESOURCE_CREATED ||
            requestResult == OC_STACK_RESOURCE_DELETED ||
            requestResult == OC_STACK_INVALID_DEVICE_INFO)
    {
        goto exit;
    }
    else if(requestResult == OC_STACK_NO_MEMORY ||
            requestResult == OC_STACK_ERROR ||
            requestResult == OC_STACK_NOTIMPL ||
            requestResult == OC_STACK_NO_RESOURCE ||
            requestResult == OC_STACK_RESOURCE_ERROR)
    {
        // TODO: should we send an error also when we receive a non-secured request to a secure resource?
        // TODO: should we consider some sort of error response
        OC_LOG(DEBUG, TAG, PCF("We should send some sort of error message"));
        // generate the pdu, if the request was CON, then the response is ACK, otherwire NON
        sendPdu = GenerateCoAPPdu((rcvdPdu->hdr->type == COAP_MESSAGE_CON)? COAP_MESSAGE_ACK : COAP_MESSAGE_NON,
                OCToCoAPResponseCode(requestResult), rcvdPdu->hdr->id,
                &protocolRequest.requestToken, NULL, NULL);
        VERIFY_NON_NULL(sendPdu);
        coap_show_pdu(sendPdu);
        sendFlag = (coap_send_flags_t)(rcvdRequest->secure ? SEND_SECURE_PORT : 0);
        if(SendCoAPPdu(gCoAPCtx, (coap_address_t*) &(rcvdRequest->remote), sendPdu,
                sendFlag)
                != OC_STACK_OK){
            OC_LOG(DEBUG, TAG, PCF("A problem occurred in sending a pdu"));
        }
        goto exit;
    }
    else if(requestResult == OC_STACK_SLOW_RESOURCE)
    {
        if(rcvdPdu->hdr->type == COAP_MESSAGE_CON)
        {
            // generate the pdu, if the request was CON, then the response is ACK, otherwire NON
            sendPdu = GenerateCoAPPdu(COAP_MESSAGE_ACK, 0, rcvdPdu->hdr->id,
                    NULL, NULL, NULL);
            VERIFY_NON_NULL(sendPdu);
            coap_show_pdu(sendPdu);

            sendFlag = (coap_send_flags_t)(rcvdRequest->secure ? SEND_SECURE_PORT : 0);
            if(SendCoAPPdu(gCoAPCtx, (coap_address_t*) &(rcvdRequest->remote), sendPdu,
                    sendFlag)
                    != OC_STACK_OK){
                OC_LOG(DEBUG, TAG, PCF("A problem occurred in sending a pdu"));
            }
        }
        else
        {
            goto exit;
        }
    }
exit:
    return;
}

uint32_t GetTime(float afterSeconds)
{
    coap_tick_t now;
    coap_ticks(&now);
    return now + (uint32_t)(afterSeconds * COAP_TICKS_PER_SECOND);
}

//This function is called back by libcoap when a response is received
static void HandleCoAPResponses(struct coap_context_t *ctx,
        const coap_queue_t * rcvdResponse) {
    OCStackResult result = OC_STACK_OK;
#ifdef CA_INT
    CAToken_t  rcvdToken = NULL;
#else // CA_INT
    OCCoAPToken rcvdToken = {{0}};
#endif // CA_INT
    OCResponse * response = NULL;
    OCClientResponse * clientResponse = NULL;
    unsigned char bufRes[MAX_RESPONSE_LENGTH] = {0};
    coap_pdu_t * sendPdu = NULL;
    coap_pdu_t * recvPdu = NULL;
    uint8_t remoteIpAddr[4] = {0};
    uint16_t remotePortNu = 0;
    uint32_t sequenceNumber = OC_OBSERVE_NO_OPTION;
    uint32_t maxAge = 0;
    unsigned char fullUri[MAX_URI_LENGTH] = { 0 };
    unsigned char rcvdUri[MAX_URI_LENGTH] = { 0 };
    coap_block_t rcvdBlock1 = {COAP_BLOCK_FILL_VALUE};
    coap_block_t rcvdBlock2 = {COAP_BLOCK_FILL_VALUE};
    uint16_t rcvdSize2 = 0;

    VERIFY_NON_NULL(ctx);
    VERIFY_NON_NULL(rcvdResponse);
    recvPdu = rcvdResponse->pdu;

    clientResponse = (OCClientResponse *) OCCalloc(1, sizeof(OCClientResponse));

    result = ParseCoAPPdu(recvPdu, rcvdUri, NULL, &sequenceNumber, &maxAge,
            &(clientResponse->numRcvdVendorSpecificHeaderOptions),
            clientResponse->rcvdVendorSpecificHeaderOptions,
            &rcvdBlock1, &rcvdBlock2, NULL, &rcvdSize2, bufRes);
    VERIFY_SUCCESS(result, OC_STACK_OK);

    // get the address of the remote
    OCDevAddrToIPv4Addr((OCDevAddr *) &(rcvdResponse->remote), remoteIpAddr,
            remoteIpAddr + 1, remoteIpAddr + 2, remoteIpAddr + 3);
    OCDevAddrToPort((OCDevAddr *) &(rcvdResponse->remote), &remotePortNu);
    snprintf((char *)fullUri, MAX_URI_LENGTH, "coap://%d.%d.%d.%d:%d%s",
            remoteIpAddr[0],remoteIpAddr[1],remoteIpAddr[2],remoteIpAddr[3],
            remotePortNu,rcvdUri);

    // fill OCCoAPToken structure
    RetrieveOCCoAPToken(recvPdu, &rcvdToken);
#ifdef CA_INT
#else
    OC_LOG_V(INFO, TAG,"Received a pdu with Token", rcvdToken.tokenLength);
    OC_LOG_BUFFER(INFO, TAG, rcvdToken.token, rcvdToken.tokenLength);
#endif

    // fill OCClientResponse structure
    result = FormOCClientResponse(clientResponse, CoAPToOCResponseCode(recvPdu->hdr->code),
            (OCDevAddr *) &(rcvdResponse->remote), sequenceNumber, NULL);
    VERIFY_SUCCESS(result, OC_STACK_OK);


    result = FormOCResponse(&response, NULL, maxAge, fullUri, rcvdUri,
            &rcvdToken, clientResponse, bufRes);
    VERIFY_SUCCESS(result, OC_STACK_OK);

    result = HandleStackResponses(response);

    if(result == OC_STACK_ERROR)
    {
        OC_LOG(INFO, TAG, PCF("Received a notification or response that is malformed or incorrect \
                         ------------ sending RESET"));
        sendPdu = GenerateCoAPPdu(COAP_MESSAGE_RST, 0,
                recvPdu->hdr->id, NULL, NULL, NULL);
        VERIFY_NON_NULL(sendPdu);
        result = SendCoAPPdu(gCoAPCtx, (coap_address_t*) &rcvdResponse->remote, sendPdu,
                     (coap_send_flags_t)(rcvdResponse->secure ? SEND_SECURE_PORT : 0));
        VERIFY_SUCCESS(result, OC_STACK_OK);
    }
    else if(result == OC_STACK_NO_MEMORY)
    {
        OC_LOG(ERROR, TAG, PCF("Received a notification or response. While processing, local " \
                "platform or memory pool ran out memory."));
    }

    if(recvPdu->hdr->type == COAP_MESSAGE_CON)
    {
        sendPdu = GenerateCoAPPdu(COAP_MESSAGE_ACK, 0,
                recvPdu->hdr->id, NULL, NULL, NULL);
        VERIFY_NON_NULL(sendPdu);
        result = SendCoAPPdu(gCoAPCtx, (coap_address_t*) &rcvdResponse->remote,
                sendPdu,
                (coap_send_flags_t)(rcvdResponse->secure ? SEND_SECURE_PORT : 0));
        VERIFY_SUCCESS(result, OC_STACK_OK);
    }

    exit:
        OCFree(response);
        OCFree(clientResponse);
}

//=============================================================================
// Functions
//=============================================================================

/**
 * Initialize the CoAP client or server with its IPv4 address and CoAP port
 *
 * @param ipAddr
 *     IP Address of host device
 * @param port
 *     Port of host device
 * @param mode
 *     Host device is client, server, or client-server
 *
 * @return
 *   0   - success
 *   TBD - TBD error
 */
OCStackResult OCInitCoAP(const char *address, uint16_t port, OCMode mode) {

    OCStackResult ret = OC_STACK_ERROR;

    TODO ("Below should go away and be replaced by OC_LOG");
    coap_log_t log_level = (coap_log_t)(LOG_DEBUG + 1);
    OCDevAddr mcastAddr;
    uint8_t ipAddr[4] = { 0 };
    uint16_t parsedPort = 0;

    OC_LOG(INFO, TAG, PCF("Entering OCInitCoAP"));

    coap_set_log_level(log_level);

    if (address)
    {
        if (!ParseIPv4Address((unsigned char *) address, ipAddr, &parsedPort))
        {
            ret = OC_STACK_ERROR;
            goto exit;
        }

        OC_LOG_V(INFO, TAG, "Parsed IP Address %d.%d.%d.%d",
                               ipAddr[0],ipAddr[1],ipAddr[2],ipAddr[3]);
    }

    gCoAPCtx = coap_new_context(ipAddr, port);
    VERIFY_NON_NULL(gCoAPCtx);

    // To allow presence notification work we need to init socket gCoAPCtx->sockfd_wellknown
    // for servers as well as clients
    OCBuildIPv4Address(COAP_WK_IPAddr_0, COAP_WK_IPAddr_1, COAP_WK_IPAddr_2,
            COAP_WK_IPAddr_3, COAP_DEFAULT_PORT, &mcastAddr);
    VERIFY_SUCCESS(
            coap_join_wellknown_group(gCoAPCtx,
                    (coap_address_t* )&mcastAddr), 0);

    coap_register_request_handler(gCoAPCtx, HandleCoAPRequests);
    coap_register_response_handler(gCoAPCtx, HandleCoAPResponses);
    coap_register_ack_rst_handler(gCoAPCtx, HandleCoAPAckRst);

    ret = OC_STACK_OK;

exit:
    if (ret != OC_STACK_OK)
    {
        OCStopCoAP();
    }
    return ret;
}

/**
 * Discover OC resources
 *
 * @param method          - method to perform on the resource
 * @param qos             - Quality of Service the request will be sent on
 * @param token           - token which will added to the request
 * @param Uri             - URI of the resource to interact with
 * @param payload         - the request payload to be added to the request before sending
 *                          by the stack when discovery or resource interaction is complete
 * @param options         - The address of an array containing the vendor specific
 *                          header options to be sent with the request
 * @return
 *   0   - success
 *   TBD - TBD error
 */
#ifdef CA_INT
OCStackResult OCDoCoAPResource(OCMethod method, OCQualityOfService qos, CAToken_t * token,
                     const char *Uri, const char *payload, OCHeaderOption * options, uint8_t numOptions)
#else
OCStackResult OCDoCoAPResource(OCMethod method, OCQualityOfService qos, OCCoAPToken * token,
                     const char *Uri, const char *payload, OCHeaderOption * options, uint8_t numOptions)
#endif
{

    OCStackResult ret = OC_STACK_ERROR;
    coap_pdu_t *pdu = NULL;
    coap_uri_t uri;
    OCDevAddr dst;
    uint8_t ipAddr[4] = { 0 };
    uint16_t port = 0;
    coap_list_t *optList = NULL;
    uint8_t coapMsgType;
    uint8_t coapMethod;
    uint32_t observeOption;
    coap_send_flags_t flag = (coap_send_flags_t)0;

    OC_LOG(INFO, TAG, PCF("Entering OCDoCoAPResource"));

    if (Uri) {
        OC_LOG_V(INFO, TAG, "URI = %s", Uri);
        VERIFY_SUCCESS(coap_split_uri((unsigned char * )Uri, strlen(Uri), &uri), OC_STACK_OK);

        // Generate the destination address
        if (uri.host.length && ParseIPv4Address(uri.host.s, ipAddr, &port)) {
            OCBuildIPv4Address(ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3], uri.port,
                    &dst);
        } else {
            goto exit;
        }

        VERIFY_SUCCESS(FormOptionList(&optList, NULL, NULL, NULL,
                (uint16_t*)&uri.port, uri.path.length, uri.path.s, uri.query.length,
                uri.query.s, options, numOptions), OC_STACK_OK);

        //TODO : Investigate the scenario where there will be no uri for OCDoCoAPResource
        flag = (coap_send_flags_t) (uri.secure ? SEND_SECURE_PORT : 0);
        OC_LOG_V(DEBUG, TAG, "uri.host.s %s", uri.host.s);
        OC_LOG_V(DEBUG, TAG, "uri.path.s %s", uri.path.s);
        OC_LOG_V(DEBUG, TAG, "uri.port %d", uri.port);
        OC_LOG_V(DEBUG, TAG, "uri.query.s %s", uri.query.s);
        OC_LOG_V(DEBUG, TAG, "secure uri %d", uri.secure);
    }

    coapMsgType = OCToCoAPQoS(qos, ipAddr);

    // Decide method type
    switch (method) {
        case OC_REST_GET:
            coapMethod = COAP_REQUEST_GET;
            break;
        case OC_REST_PUT:
            coapMethod = COAP_REQUEST_PUT;
            break;
        case OC_REST_POST:
            coapMethod = COAP_REQUEST_POST;
            break;
        case OC_REST_DELETE:
            coapMethod = COAP_REQUEST_DELETE;
            break;
        case OC_REST_OBSERVE_ALL:
        case OC_REST_OBSERVE:
        case OC_REST_CANCEL_OBSERVE:
            coapMethod = COAP_REQUEST_GET;
            observeOption = (method == OC_REST_CANCEL_OBSERVE)?
                    OC_OBSERVE_DEREGISTER:OC_OBSERVE_REGISTER;
            coap_insert(&optList, CreateNewOptionNode(COAP_OPTION_OBSERVE,
                        sizeof(observeOption), (uint8_t *)&observeOption), OrderOptions);
            break;
        default:
            coapMethod = OC_REST_NOMETHOD;
            OC_LOG(FATAL, TAG, PCF("OCDoCoAPResource only supports GET, PUT, & OBSERVE methods"));
            break;
    }

    VERIFY_NON_NULL(gCoAPCtx);
    pdu = GenerateCoAPPdu(coapMsgType, coapMethod,
            coap_new_message_id(gCoAPCtx), token,
            (unsigned char*) payload, optList);
    VERIFY_NON_NULL(pdu);

    ret = SendCoAPPdu(gCoAPCtx, (coap_address_t*) &dst, pdu, flag);

exit:
    if (ret!= OC_STACK_OK)
    {
        OC_LOG(DEBUG, TAG, PCF("A problem occurred in sending a pdu"));
    }
    return ret;
}

OCStackResult OCDoCoAPResponse(OCServerProtocolResponse *response)
{
    OCStackResult result = OC_STACK_ERROR;
    coap_pdu_t * sendPdu = NULL;
    coap_list_t *optList = NULL;
    uint8_t msgType = COAP_MESSAGE_NON;
    uint8_t mediaType = COAP_MEDIATYPE_APPLICATION_JSON;
    uint32_t maxAge = 0x2ffff;
    coap_send_flags_t sendFlag = (coap_send_flags_t)0;

    //uint32_t observeOption = OC_OBSERVE_NO_OPTION;
    //OCStackResult responseResult;

    OC_LOG(INFO, TAG, PCF("Entering OCDoCoAPResponse"));

    if(response->notificationFlag && response->qos == OC_HIGH_QOS)
    {
        msgType = COAP_MESSAGE_CON;
    }
    else if(response->notificationFlag && response->qos != OC_HIGH_QOS)
    {
        msgType = COAP_MESSAGE_NON;
    }
    else if(!response->notificationFlag && !response->slowFlag && response->qos == OC_HIGH_QOS)
    {
        msgType = COAP_MESSAGE_ACK;
    }
    else if(!response->notificationFlag && response->slowFlag && response->qos == OC_HIGH_QOS)
    {
        msgType = COAP_MESSAGE_CON;
    }
    else if(!response->notificationFlag)
    {
        msgType = COAP_MESSAGE_NON;
    }

    if(response->coapID == 0)
    {
        response->coapID = coap_new_message_id(gCoAPCtx);
    }

    if (response->observationOption != OC_OBSERVE_NO_OPTION)
    {
        result = FormOptionList(&optList, &mediaType, &maxAge,
                &response->observationOption, NULL,
                strlen((char *)response->resourceUri), response->resourceUri,
                0, NULL,
                response->sendVendorSpecificHeaderOptions,
                response->numSendVendorSpecificHeaderOptions);
    }
    else
    {
        result = FormOptionList(&optList, &mediaType, &maxAge,
                NULL, NULL,
                strlen((char *)response->resourceUri), response->resourceUri,
                0, NULL,
                response->sendVendorSpecificHeaderOptions,
                response->numSendVendorSpecificHeaderOptions);
    }
    VERIFY_SUCCESS(result, OC_STACK_OK);

    sendPdu = GenerateCoAPPdu(msgType, OCToCoAPResponseCode(response->result),
            response->coapID, response->requestToken, (unsigned char *)response->payload,
            optList);

    VERIFY_NON_NULL(sendPdu);
    coap_show_pdu(sendPdu);

    sendFlag = (coap_send_flags_t)(response->delayedResNeeded ? SEND_DELAYED : 0);
    sendFlag = (coap_send_flags_t)( sendFlag | (response->secured ? SEND_SECURE_PORT : 0));

    if (SendCoAPPdu(gCoAPCtx, (coap_address_t *) (response->requesterAddr), sendPdu, sendFlag)
            != OC_STACK_OK)
    {
        OC_LOG(ERROR, TAG, PCF("A problem occurred in sending a pdu"));
        return OC_STACK_ERROR;
    }
    return OC_STACK_OK;
exit:
    OC_LOG(ERROR, TAG, PCF("Error formatting server response"));
    return OC_STACK_ERROR;
}

/**
 * Stop the CoAP client or server processing
 *
 * @return 0 - success, else - TBD error
 */
OCStackResult OCStopCoAP() {
    OC_LOG(INFO, TAG, PCF("Entering OCStopCoAP"));
    coap_free_context(gCoAPCtx);
    gCoAPCtx = NULL;
    return OC_STACK_OK;
}

/**
 * Called in main loop of CoAP client or server.  Allows low-level CoAP processing of
 * send, receive, timeout, discovery, callbacks, etc.
 *
 * @return 0 - success, else - TBD error
 */
OCStackResult OCProcessCoAP() {
    int read = 0;
    read = coap_read(gCoAPCtx, gCoAPCtx->sockfd);
    if(read > 0) {
        OC_LOG(INFO, TAG, PCF("This is a Unicast<============"));
    }
    if (-1 != gCoAPCtx->sockfd_wellknown) {
        read = coap_read(gCoAPCtx, gCoAPCtx->sockfd_wellknown);
        if(read > 0)
        {
            OC_LOG(INFO, TAG, PCF("This is a Multicast<==========="));
        }
    }
    if (-1 != gCoAPCtx->sockfd_dtls) {
        read = coap_read(gCoAPCtx, gCoAPCtx->sockfd_dtls);
        if(read > 0)
        {
            OC_LOG(INFO, TAG, PCF("This is a Secure packet<==========="));
        }
    }
    coap_dispatch(gCoAPCtx);

    HandleSendQueue(gCoAPCtx);

    return OC_STACK_OK;
}


/**
 * Retrieve the info about the end-point where resource is being hosted.
 * Currently, this method only provides the IP port with which the socket
 * is bound.
 *
 * @return 0 - success, else - TBD error
 */
OCStackResult OCGetResourceEndPointInfo (OCResource *resPtr, void *info) {

    OCStackResult result = OC_STACK_ERROR;
    int sfd;
    OC_LOG(INFO, TAG, PCF("Entering OCGetResourceEndPointInfo"));
    VERIFY_NON_NULL(resPtr);
    VERIFY_NON_NULL(info);

    sfd = (resPtr->resourceProperties & OC_SECURE) ? gCoAPCtx->sockfd_dtls :
            gCoAPCtx->sockfd;

    if (OCGetSocketInfo(sfd, (uint16_t*)info) == ERR_SUCCESS)
        result = OC_STACK_OK;
exit:
    return result;
}


