/*
 * Copyright (C) 1996-2023 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 88    Client-side Reply Routines */

#include "squid.h"
#include "acl/FilledChecklist.h"
#include "acl/Gadgets.h"
#include "anyp/PortCfg.h"
#include "client_side_reply.h"
#include "errorpage.h"
#include "ETag.h"
#include "fd.h"
#include "fde.h"
#include "format/Token.h"
#include "FwdState.h"
#include "globals.h"
#include "http/Stream.h"
#include "HttpHeaderTools.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "ip/QosConfig.h"
#include "ipcache.h"
#include "log/access_log.h"
#include "MemObject.h"
#include "mime_header.h"
#include "neighbors.h"
#include "refresh.h"
#include "RequestFlags.h"
#include "SquidConfig.h"
#include "Store.h"
#include "StrList.h"
#include "tools.h"
#if USE_AUTH
#include "auth/UserRequest.h"
#endif
#if USE_DELAY_POOLS
#include "DelayPools.h"
#endif
#if USE_SQUID_ESI
#include "esi/Esi.h"
#endif

#include <memory>

CBDATA_CLASS_INIT(clientReplyContext);

/* Local functions */
extern "C" CSS clientReplyStatus;
ErrorState *clientBuildError(err_type, Http::StatusCode, char const *, const ConnStateData *, HttpRequest *, const AccessLogEntry::Pointer &);

/* privates */

clientReplyContext::~clientReplyContext()
{
    deleting = true;
    /* This may trigger a callback back into SendMoreData as the cbdata
     * is still valid
     */
    removeClientStoreReference(&sc, http);
    /* old_entry might still be set if we didn't yet get the reply
     * code in HandleIMSReply() */
    removeStoreReference(&old_sc, &old_entry);
    cbdataReferenceDone(http);
    HTTPMSGUNLOCK(reply);
}

clientReplyContext::clientReplyContext(ClientHttpRequest *clientContext) :
    purgeStatus(Http::scNone),
    http(cbdataReference(clientContext)),
    headers_sz(0),
    sc(nullptr),
    reqsize(0),
    reqofs(0),
    ourNode(nullptr),
    reply(nullptr),
    old_entry(nullptr),
    old_sc(nullptr),
    old_lastmod(-1),
    old_reqofs(0),
    old_reqsize(0),
    deleting(false),
    collapsedRevalidation(crNone)
{
    *tempbuf = 0;
}

/** Create an error in the store awaiting the client side to read it.
 *
 * This may be better placed in the clientStream logic, but it has not been
 * relocated there yet
 */
void
clientReplyContext::setReplyToError(
    err_type err, Http::StatusCode status, char const *uri,
    const ConnStateData *conn, HttpRequest *failedrequest, const char *unparsedrequest,
#if USE_AUTH
    Auth::UserRequest::Pointer auth_user_request
#else
    void*
#endif
)
{
    auto errstate = clientBuildError(err, status, uri, conn, failedrequest, http->al);

    if (unparsedrequest)
        errstate->request_hdrs = xstrdup(unparsedrequest);

#if USE_AUTH
    errstate->auth_user_request = auth_user_request;
#endif
    setReplyToError(failedrequest ? failedrequest->method : HttpRequestMethod(Http::METHOD_NONE), errstate);
}

void clientReplyContext::setReplyToError(const HttpRequestMethod& method, ErrorState *errstate)
{
    if (errstate->httpStatus == Http::scNotImplemented && http->request)
        /* prevent confusion over whether we default to persistent or not */
        http->request->flags.proxyKeepalive = false;

    http->al->http.code = errstate->httpStatus;

    if (http->request)
        http->request->ignoreRange("responding with a Squid-generated error");

    createStoreEntry(method, RequestFlags());
    assert(errstate->callback_data == nullptr);
    errorAppendEntry(http->storeEntry(), errstate);
    /* Now the caller reads to get this */
}

void
clientReplyContext::setReplyToReply(HttpReply *futureReply)
{
    Must(futureReply);
    http->al->http.code = futureReply->sline.status();

    HttpRequestMethod method;
    if (http->request) { // nil on responses to unparsable requests
        http->request->ignoreRange("responding with a Squid-generated reply");
        method = http->request->method;
    }

    createStoreEntry(method, RequestFlags());

    http->storeEntry()->storeErrorResponse(futureReply);
    /* Now the caller reads to get futureReply */
}

// Assumes that the entry contains an error response without Content-Range.
// To use with regular entries, make HTTP Range header removal conditional.
void clientReplyContext::setReplyToStoreEntry(StoreEntry *entry, const char *reason)
{
    entry->lock("clientReplyContext::setReplyToStoreEntry"); // removeClientStoreReference() unlocks
    sc = storeClientListAdd(entry, this);
#if USE_DELAY_POOLS
    sc->setDelayId(DelayId::DelayClient(http));
#endif
    reqofs = 0;
    reqsize = 0;
    if (http->request)
        http->request->ignoreRange(reason);
    flags.storelogiccomplete = 1;
    http->storeEntry(entry);
}

void
clientReplyContext::removeStoreReference(store_client ** scp,
        StoreEntry ** ep)
{
    StoreEntry *e;
    store_client *sc_tmp = *scp;

    if ((e = *ep) != nullptr) {
        *ep = nullptr;
        storeUnregister(sc_tmp, e, this);
        *scp = nullptr;
        e->unlock("clientReplyContext::removeStoreReference");
    }
}

void
clientReplyContext::removeClientStoreReference(store_client **scp, ClientHttpRequest *aHttpRequest)
{
    StoreEntry *reference = aHttpRequest->storeEntry();
    removeStoreReference(scp, &reference);
    aHttpRequest->storeEntry(reference);
}

void
clientReplyContext::saveState()
{
    assert(old_sc == nullptr);
    debugs(88, 3, "clientReplyContext::saveState: saving store context");
    old_entry = http->storeEntry();
    old_sc = sc;
    old_lastmod = http->request->lastmod;
    old_etag = http->request->etag;
    old_reqsize = reqsize;
    old_reqofs = reqofs;
    /* Prevent accessing the now saved entries */
    http->storeEntry(nullptr);
    sc = nullptr;
    reqsize = 0;
    reqofs = 0;
}

void
clientReplyContext::restoreState()
{
    assert(old_sc != nullptr);
    debugs(88, 3, "clientReplyContext::restoreState: Restoring store context");
    removeClientStoreReference(&sc, http);
    http->storeEntry(old_entry);
    sc = old_sc;
    reqsize = old_reqsize;
    reqofs = old_reqofs;
    http->request->lastmod = old_lastmod;
    http->request->etag = old_etag;
    /* Prevent accessed the old saved entries */
    old_entry = nullptr;
    old_sc = nullptr;
    old_lastmod = -1;
    old_etag.clean();
    old_reqsize = 0;
    old_reqofs = 0;
}

void
clientReplyContext::startError(ErrorState * err)
{
    createStoreEntry(http->request->method, RequestFlags());
    triggerInitialStoreRead();
    errorAppendEntry(http->storeEntry(), err);
}

clientStreamNode *
clientReplyContext::getNextNode() const
{
    return (clientStreamNode *)ourNode->node.next->data;
}

/* This function is wrong - the client parameters don't include the
 * header offset
 */
void
clientReplyContext::triggerInitialStoreRead()
{
    /* when confident, 0 becomes reqofs, and then this factors into
     * startSendProcess
     */
    assert(reqofs == 0);
    StoreIOBuffer localTempBuffer (next()->readBuffer.length, 0, next()->readBuffer.data);
    storeClientCopy(sc, http->storeEntry(), localTempBuffer, SendMoreData, this);
}

/* there is an expired entry in the store.
 * setup a temporary buffer area and perform an IMS to the origin
 */
void
clientReplyContext::processExpired()
{
    const char *url = storeId();
    debugs(88, 3, "clientReplyContext::processExpired: '" << http->uri << "'");
    const time_t lastmod = http->storeEntry()->lastModified();
    assert(lastmod >= 0);
    /*
     * check if we are allowed to contact other servers
     * @?@: Instead of a 504 (Gateway Timeout) reply, we may want to return
     *      a stale entry *if* it matches client requirements
     */

    if (http->onlyIfCached()) {
        processOnlyIfCachedMiss();
        return;
    }

    http->updateLoggingTags(LOG_TCP_REFRESH);
    http->request->flags.refresh = true;
#if STORE_CLIENT_LIST_DEBUG
    /* Prevent a race with the store client memory free routines
     */
    assert(storeClientIsThisAClient(sc, this));
#endif
    /* Prepare to make a new temporary request */
    saveState();

    // TODO: Consider also allowing regular (non-collapsed) revalidation hits.
    // TODO: support collapsed revalidation for Vary-controlled entries
    bool collapsingAllowed = Config.onoff.collapsed_forwarding &&
                             !Store::Controller::SmpAware() &&
                             http->request->vary_headers.isEmpty();

    StoreEntry *entry = nullptr;
    if (collapsingAllowed) {
        if (const auto e = storeGetPublicByRequest(http->request, ksRevalidation)) {
            if (e->hittingRequiresCollapsing() && startCollapsingOn(*e, true)) {
                entry = e;
                entry->lock("clientReplyContext::processExpired#alreadyRevalidating");
            } else {
                e->abandon(__FUNCTION__);
                // assume mayInitiateCollapsing() would fail too
                collapsingAllowed = false;
            }
        }
    }

    if (entry) {
        entry->ensureMemObject(url, http->log_uri, http->request->method);
        debugs(88, 5, "collapsed on existing revalidation entry: " << *entry);
        collapsedRevalidation = crSlave;
    } else {
        entry = storeCreateEntry(url,
                                 http->log_uri, http->request->flags, http->request->method);
        /* NOTE, don't call StoreEntry->lock(), storeCreateEntry() does it */

        if (collapsingAllowed && mayInitiateCollapsing() &&
                Store::Root().allowCollapsing(entry, http->request->flags, http->request->method)) {
            debugs(88, 5, "allow other revalidation requests to collapse on " << *entry);
            collapsedRevalidation = crInitiator;
        } else {
            collapsedRevalidation = crNone;
        }
    }

    sc = storeClientListAdd(entry, this);
#if USE_DELAY_POOLS
    /* delay_id is already set on original store client */
    sc->setDelayId(DelayId::DelayClient(http));
#endif

    http->request->lastmod = lastmod;

    if (!http->request->header.has(Http::HdrType::IF_NONE_MATCH)) {
        ETag etag = {nullptr, -1}; // TODO: make that a default ETag constructor
        if (old_entry->hasEtag(etag) && !etag.weak)
            http->request->etag = etag.str;
    }

    debugs(88, 5, "lastmod " << entry->lastModified());
    http->storeEntry(entry);
    assert(http->out.offset == 0);
    assert(http->request->clientConnectionManager == http->getConn());

    if (collapsedRevalidation != crSlave) {
        /*
         * A refcounted pointer so that FwdState stays around as long as
         * this clientReplyContext does
         */
        Comm::ConnectionPointer conn = http->getConn() != nullptr ? http->getConn()->clientConnection : nullptr;
        FwdState::Start(conn, http->storeEntry(), http->request, http->al);
    }
    /* Register with storage manager to receive updates when data comes in. */

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED))
        debugs(88, DBG_CRITICAL, "clientReplyContext::processExpired: Found ENTRY_ABORTED object");

    {
        /* start counting the length from 0 */
        StoreIOBuffer localTempBuffer(HTTP_REQBUF_SZ, 0, tempbuf);
        storeClientCopy(sc, entry, localTempBuffer, HandleIMSReply, this);
    }
}

void
clientReplyContext::sendClientUpstreamResponse()
{
    StoreIOBuffer tempresult;
    removeStoreReference(&old_sc, &old_entry);

    if (collapsedRevalidation)
        http->storeEntry()->clearPublicKeyScope();

    /* here the data to send is the data we just received */
    old_reqofs = 0;
    old_reqsize = 0;
    /* sendMoreData tracks the offset as well.
     * Force it back to zero */
    reqofs = 0;
    assert(!EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED));
    /* TODO: provide sendMoreData with the ready parsed reply */
    tempresult.length = reqsize;
    tempresult.data = tempbuf;
    sendMoreData(tempresult);
}

void
clientReplyContext::HandleIMSReply(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = (clientReplyContext *)data;
    context->handleIMSReply(result);
}

void
clientReplyContext::sendClientOldEntry()
{
    /* Get the old request back */
    restoreState();
    /* here the data to send is in the next nodes buffers already */
    assert(!EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED));
    /* sendMoreData tracks the offset as well.
     * Force it back to zero */
    reqofs = 0;
    StoreIOBuffer tempresult (reqsize, reqofs, next()->readBuffer.data);
    sendMoreData(tempresult);
}

/* This is the workhorse of the HandleIMSReply callback.
 *
 * It is called when we've got data back from the origin following our
 * IMS request to revalidate a stale entry.
 */
void
clientReplyContext::handleIMSReply(StoreIOBuffer result)
{
    if (deleting)
        return;

    debugs(88, 3, http->storeEntry()->url() << ", " << (long unsigned) result.length << " bytes");

    if (http->storeEntry() == nullptr)
        return;

    if (result.flags.error && !EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED))
        return;

    if (collapsedRevalidation == crSlave && !http->storeEntry()->mayStartHitting()) {
        debugs(88, 3, "CF slave hit private non-shareable " << *http->storeEntry() << ". MISS");
        // restore context to meet processMiss() expectations
        restoreState();
        http->updateLoggingTags(LOG_TCP_MISS);
        processMiss();
        return;
    }

    /* update size of the request */
    reqsize = result.length + reqofs;

    // request to origin was aborted
    if (EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) {
        debugs(88, 3, "request to origin aborted '" << http->storeEntry()->url() << "', sending old entry to client");
        http->updateLoggingTags(LOG_TCP_REFRESH_FAIL_OLD);
        sendClientOldEntry();
        return;
    }

    const auto oldStatus = old_entry->mem().freshestReply().sline.status();
    const auto &new_rep = http->storeEntry()->mem().freshestReply();
    const auto status = new_rep.sline.status();

    // XXX: Disregard stale incomplete (i.e. still being written) borrowed (i.e.
    // not caused by our request) IMS responses. That new_rep may be very old!

    // origin replied 304
    if (status == Http::scNotModified) {
        http->updateLoggingTags(LOG_TCP_REFRESH_UNMODIFIED);
        http->request->flags.staleIfHit = false; // old_entry is no longer stale

        // TODO: The update may not be instantaneous. Should we wait for its
        // completion to avoid spawning too much client-disassociated work?
        Store::Root().updateOnNotModified(old_entry, *http->storeEntry());

        // if client sent IMS
        if (http->request->flags.ims && !old_entry->modifiedSince(http->request->ims, http->request->imslen)) {
            // forward the 304 from origin
            debugs(88, 3, "origin replied 304, revalidated existing entry and forwarding 304 to client");
            sendClientUpstreamResponse();
            return;
        }

        // send existing entry, it's still valid
        debugs(88, 3, "origin replied 304, revalidated existing entry and sending " << oldStatus << " to client");
        sendClientOldEntry();
        return;
    }

    // origin replied with a non-error code
    if (status > Http::scNone && status < Http::scInternalServerError) {
        // RFC 9111 section 4:
        // "When more than one suitable response is stored,
        //  a cache MUST use the most recent one
        // (as determined by the Date header field)."
        if (new_rep.olderThan(&old_entry->mem().freshestReply())) {
            http->al->cache.code.err.ignored = true;
            debugs(88, 3, "origin replied " << status << " but with an older date header, sending old entry (" << oldStatus << ") to client");
            sendClientOldEntry();
            return;
        }

        http->updateLoggingTags(LOG_TCP_REFRESH_MODIFIED);
        debugs(88, 3, "origin replied " << status << ", forwarding to client");
        sendClientUpstreamResponse();
        return;
    }

    // origin replied with an error
    if (http->request->flags.failOnValidationError) {
        http->updateLoggingTags(LOG_TCP_REFRESH_FAIL_ERR);
        debugs(88, 3, "origin replied with error " << status << ", forwarding to client due to fail_on_validation_err");
        sendClientUpstreamResponse();
        return;
    }

    // ignore and let client have old entry
    http->updateLoggingTags(LOG_TCP_REFRESH_FAIL_OLD);
    debugs(88, 3, "origin replied with error " << status << ", sending old entry (" << oldStatus << ") to client");
    sendClientOldEntry();
}

SQUIDCEXTERN CSR clientGetMoreData;
SQUIDCEXTERN CSD clientReplyDetach;

/**
 * clientReplyContext::cacheHit Should only be called until the HTTP reply headers
 * have been parsed.  Normally this should be a single call, but
 * it might take more than one.  As soon as we have the headers,
 * we hand off to clientSendMoreData, processExpired, or
 * processMiss.
 */
void
clientReplyContext::CacheHit(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = (clientReplyContext *)data;
    context->cacheHit(result);
}

/**
 * Process a possible cache HIT.
 */
void
clientReplyContext::cacheHit(StoreIOBuffer result)
{
    /** Ignore if the HIT object is being deleted. */
    if (deleting) {
        debugs(88, 3, "HIT object being deleted. Ignore the HIT.");
        return;
    }

    StoreEntry *e = http->storeEntry();

    HttpRequest *r = http->request;

    debugs(88, 3, "clientCacheHit: " << http->uri << ", " << result.length << " bytes");

    if (http->storeEntry() == nullptr) {
        debugs(88, 3, "clientCacheHit: request aborted");
        return;
    } else if (result.flags.error) {
        /* swap in failure */
        debugs(88, 3, "clientCacheHit: swapin failure for " << http->uri);
        http->updateLoggingTags(LOG_TCP_SWAPFAIL_MISS);
        removeClientStoreReference(&sc, http);
        processMiss();
        return;
    }

    // The previously identified hit suddenly became unshareable!
    // This is common for collapsed forwarding slaves but might also
    // happen to regular hits because we are called asynchronously.
    if (!e->mayStartHitting()) {
        debugs(88, 3, "unshareable " << *e << ". MISS");
        http->updateLoggingTags(LOG_TCP_MISS);
        processMiss();
        return;
    }

    if (result.length == 0) {
        debugs(88, 5, "store IO buffer has no content. MISS");
        /* the store couldn't get enough data from the file for us to id the
         * object
         */
        /* treat as a miss */
        http->updateLoggingTags(LOG_TCP_MISS);
        processMiss();
        return;
    }

    assert(!EBIT_TEST(e->flags, ENTRY_ABORTED));
    /* update size of the request */
    reqsize = result.length + reqofs;

    /*
     * Got the headers, now grok them
     */
    assert(http->loggingTags().oldType == LOG_TCP_HIT);

    if (http->request->storeId().cmp(e->mem_obj->storeId()) != 0) {
        debugs(33, DBG_IMPORTANT, "clientProcessHit: URL mismatch, '" << e->mem_obj->storeId() << "' != '" << http->request->storeId() << "'");
        http->updateLoggingTags(LOG_TCP_MISS); // we lack a more precise LOG_*_MISS code
        processMiss();
        return;
    }

    switch (varyEvaluateMatch(e, r)) {

    case VARY_NONE:
        /* No variance detected. Continue as normal */
        break;

    case VARY_MATCH:
        /* This is the correct entity for this request. Continue */
        debugs(88, 2, "clientProcessHit: Vary MATCH!");
        break;

    case VARY_OTHER:
        /* This is not the correct entity for this request. We need
         * to requery the cache.
         */
        removeClientStoreReference(&sc, http);
        e = nullptr;
        /* Note: varyEvalyateMatch updates the request with vary information
         * so we only get here once. (it also takes care of cancelling loops)
         */
        debugs(88, 2, "clientProcessHit: Vary detected!");
        clientGetMoreData(ourNode, http);
        return;

    case VARY_CANCEL:
        /* varyEvaluateMatch found a object loop. Process as miss */
        debugs(88, DBG_IMPORTANT, "clientProcessHit: Vary object loop!");
        http->updateLoggingTags(LOG_TCP_MISS); // we lack a more precise LOG_*_MISS code
        processMiss();
        return;
    }

    if (r->method == Http::METHOD_PURGE) {
        debugs(88, 5, "PURGE gets a HIT");
        removeClientStoreReference(&sc, http);
        e = nullptr;
        purgeRequest();
        return;
    }

    if (e->checkNegativeHit() && !r->flags.noCacheHack()) {
        debugs(88, 5, "negative-HIT");
        http->updateLoggingTags(LOG_TCP_NEGATIVE_HIT);
        sendMoreData(result);
        return;
    } else if (blockedHit()) {
        debugs(88, 5, "send_hit forces a MISS");
        http->updateLoggingTags(LOG_TCP_MISS);
        processMiss();
        return;
    } else if (!http->flags.internal && refreshCheckHTTP(e, r)) {
        debugs(88, 5, "clientCacheHit: in refreshCheck() block");
        /*
         * We hold a stale copy; it needs to be validated
         */
        /*
         * The 'needValidation' flag is used to prevent forwarding
         * loops between siblings.  If our copy of the object is stale,
         * then we should probably only use parents for the validation
         * request.  Otherwise two siblings could generate a loop if
         * both have a stale version of the object.
         */
        r->flags.needValidation = true;

        if (e->lastModified() < 0) {
            debugs(88, 3, "validate HIT object? NO. Can't calculate entry modification time. Do MISS.");
            /*
             * We cannot revalidate entries without knowing their
             * modification time.
             * XXX: BUG 1890 objects without Date do not get one added.
             */
            http->updateLoggingTags(LOG_TCP_MISS);
            processMiss();
        } else if (r->flags.noCache) {
            debugs(88, 3, "validate HIT object? NO. Client sent CC:no-cache. Do CLIENT_REFRESH_MISS");
            /*
             * This did not match a refresh pattern that overrides no-cache
             * we should honour the client no-cache header.
             */
            http->updateLoggingTags(LOG_TCP_CLIENT_REFRESH_MISS);
            processMiss();
        } else if (r->url.getScheme() == AnyP::PROTO_HTTP || r->url.getScheme() == AnyP::PROTO_HTTPS) {
            debugs(88, 3, "validate HIT object? YES.");
            /*
             * Object needs to be revalidated
             * XXX This could apply to FTP as well, if Last-Modified is known.
             */
            processExpired();
        } else {
            debugs(88, 3, "validate HIT object? NO. Client protocol non-HTTP. Do MISS.");
            /*
             * We don't know how to re-validate other protocols. Handle
             * them as if the object has expired.
             */
            http->updateLoggingTags(LOG_TCP_MISS);
            processMiss();
        }
        return;
    } else if (r->conditional()) {
        debugs(88, 5, "conditional HIT");
        if (processConditional())
            return;
    }

    /*
     * plain ol' cache hit
     */
    debugs(88, 5, "plain old HIT");

#if USE_DELAY_POOLS
    if (e->store_status != STORE_OK)
        http->updateLoggingTags(LOG_TCP_MISS);
    else
#endif
        if (e->mem_status == IN_MEMORY)
            http->updateLoggingTags(LOG_TCP_MEM_HIT);
        else if (Config.onoff.offline)
            http->updateLoggingTags(LOG_TCP_OFFLINE_HIT);

    sendMoreData(result);
}

/**
 * Prepare to fetch the object as it's a cache miss of some kind.
 */
void
clientReplyContext::processMiss()
{
    char *url = http->uri;
    HttpRequest *r = http->request;
    ErrorState *err = nullptr;
    debugs(88, 4, r->method << ' ' << url);

    /**
     * We might have a left-over StoreEntry from a failed cache hit
     * or IMS request.
     */
    if (http->storeEntry()) {
        if (EBIT_TEST(http->storeEntry()->flags, ENTRY_SPECIAL)) {
            debugs(88, DBG_CRITICAL, "clientProcessMiss: miss on a special object (" << url << ").");
            debugs(88, DBG_CRITICAL, "\tlog_type = " << http->loggingTags().c_str());
            http->storeEntry()->dump(1);
        }

        removeClientStoreReference(&sc, http);
    }

    /** Check if its a PURGE request to be actioned. */
    if (r->method == Http::METHOD_PURGE) {
        purgeRequest();
        return;
    }

    /** Check if its an 'OTHER' request. Purge all cached entries if so and continue. */
    if (r->method == Http::METHOD_OTHER) {
        purgeAllCached();
    }

    /** Check if 'only-if-cached' flag is set. Action if so. */
    if (http->onlyIfCached()) {
        processOnlyIfCachedMiss();
        return;
    }

    /// Deny loops
    if (r->flags.loopDetected) {
        http->al->http.code = Http::scForbidden;
        err = clientBuildError(ERR_ACCESS_DENIED, Http::scForbidden, nullptr, http->getConn(), http->request, http->al);
        createStoreEntry(r->method, RequestFlags());
        errorAppendEntry(http->storeEntry(), err);
        triggerInitialStoreRead();
        return;
    } else {
        assert(http->out.offset == 0);
        createStoreEntry(r->method, r->flags);
        triggerInitialStoreRead();

        if (http->redirect.status) {
            const HttpReplyPointer rep(new HttpReply);
            http->updateLoggingTags(LOG_TCP_REDIRECT);
            http->storeEntry()->releaseRequest();
            rep->redirect(http->redirect.status, http->redirect.location);
            http->storeEntry()->replaceHttpReply(rep);
            http->storeEntry()->complete();
            return;
        }

        assert(r->clientConnectionManager == http->getConn());

        Comm::ConnectionPointer conn = http->getConn() != nullptr ? http->getConn()->clientConnection : nullptr;
        /** Start forwarding to get the new object from network */
        FwdState::Start(conn, http->storeEntry(), r, http->al);
    }
}

/**
 * client issued a request with an only-if-cached cache-control directive;
 * we did not find a cached object that can be returned without
 *     contacting other servers;
 * respond with a 504 (Gateway Timeout) as suggested in [RFC 2068]
 */
void
clientReplyContext::processOnlyIfCachedMiss()
{
    debugs(88, 4, http->request->method << ' ' << http->uri);
    http->al->http.code = Http::scGatewayTimeout;
    ErrorState *err = clientBuildError(ERR_ONLY_IF_CACHED_MISS, Http::scGatewayTimeout, nullptr,
                                       http->getConn(), http->request, http->al);
    removeClientStoreReference(&sc, http);
    startError(err);
}

/// process conditional request from client
bool
clientReplyContext::processConditional()
{
    StoreEntry *const e = http->storeEntry();

    const auto replyStatusCode = e->mem().baseReply().sline.status();
    if (replyStatusCode != Http::scOkay) {
        debugs(88, 4, "miss because " << replyStatusCode << " != 200");
        http->updateLoggingTags(LOG_TCP_MISS);
        processMiss();
        return true;
    }

    HttpRequest &r = *http->request;

    if (r.header.has(Http::HdrType::IF_MATCH) && !e->hasIfMatchEtag(r)) {
        // RFC 2616: reply with 412 Precondition Failed if If-Match did not match
        sendPreconditionFailedError();
        return true;
    }

    if (r.header.has(Http::HdrType::IF_NONE_MATCH)) {
        // RFC 7232: If-None-Match recipient MUST ignore IMS
        r.flags.ims = false;
        r.ims = -1;
        r.imslen = 0;
        r.header.delById(Http::HdrType::IF_MODIFIED_SINCE);

        if (e->hasIfNoneMatchEtag(r)) {
            sendNotModifiedOrPreconditionFailedError();
            return true;
        }

        // None-Match is true (no ETag matched); treat as an unconditional hit
        return false;
    }

    if (r.flags.ims) {
        // handle If-Modified-Since requests from the client
        if (e->modifiedSince(r.ims, r.imslen)) {
            // Modified-Since is true; treat as an unconditional hit
            return false;

        } else {
            // otherwise reply with 304 Not Modified
            sendNotModified();
        }
        return true;
    }

    return false;
}

/// whether squid.conf send_hit prevents us from serving this hit
bool
clientReplyContext::blockedHit() const
{
    if (!Config.accessList.sendHit)
        return false; // hits are not blocked by default

    if (http->flags.internal)
        return false; // internal content "hits" cannot be blocked

    const auto &rep = http->storeEntry()->mem().freshestReply();
    {
        std::unique_ptr<ACLFilledChecklist> chl(clientAclChecklistCreate(Config.accessList.sendHit, http));
        chl->reply = const_cast<HttpReply*>(&rep); // ACLChecklist API bug
        HTTPMSGLOCK(chl->reply);
        return !chl->fastCheck().allowed(); // when in doubt, block
    }
}

// Purges all entries with a given url
// TODO: move to SideAgent parent, when we have one
/*
 * We probably cannot purge Vary-affected responses because their MD5
 * keys depend on vary headers.
 */
void
purgeEntriesByUrl(HttpRequest * req, const char *url)
{
    for (HttpRequestMethod m(Http::METHOD_NONE); m != Http::METHOD_ENUM_END; ++m) {
        if (m.respMaybeCacheable()) {
            const cache_key *key = storeKeyPublic(url, m);
            debugs(88, 5, m << ' ' << url << ' ' << storeKeyText(key));
#if USE_HTCP
            neighborsHtcpClear(nullptr, req, m, HTCP_CLR_INVALIDATION);
#else
            (void)req;
#endif
            Store::Root().evictIfFound(key);
        }
    }
}

void
clientReplyContext::purgeAllCached()
{
    // XXX: performance regression, c_str() reallocates
    SBuf url(http->request->effectiveRequestUri());
    purgeEntriesByUrl(http->request, url.c_str());
}

LogTags *
clientReplyContext::loggingTags() const
{
    // XXX: clientReplyContext code assumes that http cbdata is always valid.
    // TODO: Either add cbdataReferenceValid(http) checks in all the relevant
    // places, like this one, or remove cbdata protection of the http member.
    return &http->al->cache.code;
}

void
clientReplyContext::purgeRequest()
{
    debugs(88, 3, "Config2.onoff.enable_purge = " <<
           Config2.onoff.enable_purge);

    if (!Config2.onoff.enable_purge) {
        http->updateLoggingTags(LOG_TCP_DENIED);
        ErrorState *err = clientBuildError(ERR_ACCESS_DENIED, Http::scForbidden, nullptr,
                                           http->getConn(), http->request, http->al);
        startError(err);
        return;
    }

    /* Release both IP cache */
    ipcacheInvalidate(http->request->url.host());

    // TODO: can we use purgeAllCached() here instead?
    purgeDoPurge();
}

void
clientReplyContext::purgeDoPurge()
{
    auto firstFound = false;
    if (const auto entry = storeGetPublicByRequestMethod(http->request, Http::METHOD_GET)) {
        // special entries are only METHOD_GET entries without variance
        if (EBIT_TEST(entry->flags, ENTRY_SPECIAL)) {
            http->updateLoggingTags(LOG_TCP_DENIED);
            const auto err = clientBuildError(ERR_ACCESS_DENIED, Http::scForbidden, nullptr,
                                              http->getConn(), http->request, http->al);
            startError(err);
            entry->abandon(__FUNCTION__);
            return;
        }
        firstFound = true;
        if (!purgeEntry(*entry, Http::METHOD_GET))
            return;
    }

    detailStoreLookup(storeLookupString(firstFound));

    if (const auto entry = storeGetPublicByRequestMethod(http->request, Http::METHOD_HEAD)) {
        if (!purgeEntry(*entry, Http::METHOD_HEAD))
            return;
    }

    /* And for Vary, release the base URI if none of the headers was included in the request */
    if (!http->request->vary_headers.isEmpty()
            && http->request->vary_headers.find('=') != SBuf::npos) {
        // XXX: performance regression, c_str() reallocates
        SBuf tmp(http->request->effectiveRequestUri());

        if (const auto entry = storeGetPublic(tmp.c_str(), Http::METHOD_GET)) {
            if (!purgeEntry(*entry, Http::METHOD_GET, "Vary "))
                return;
        }

        if (const auto entry = storeGetPublic(tmp.c_str(), Http::METHOD_HEAD)) {
            if (!purgeEntry(*entry, Http::METHOD_HEAD, "Vary "))
                return;
        }
    }

    if (purgeStatus == Http::scNone)
        purgeStatus = Http::scNotFound;

    /*
     * Make a new entry to hold the reply to be written
     * to the client.
     */
    /* TODO: This doesn't need to go through the store. Simply
     * push down the client chain
     */
    createStoreEntry(http->request->method, RequestFlags());

    triggerInitialStoreRead();

    const HttpReplyPointer rep(new HttpReply);
    rep->setHeaders(purgeStatus, nullptr, nullptr, 0, 0, -1);
    http->storeEntry()->replaceHttpReply(rep);
    http->storeEntry()->complete();
}

bool
clientReplyContext::purgeEntry(StoreEntry &entry, const Http::MethodType methodType, const char *descriptionPrefix)
{
    debugs(88, 4, descriptionPrefix << Http::MethodStr(methodType) << " '" << entry.url() << "'" );
#if USE_HTCP
    neighborsHtcpClear(&entry, http->request, HttpRequestMethod(methodType), HTCP_CLR_PURGE);
#endif
    entry.release(true);
    purgeStatus = Http::scOkay;
    return true;
}

void
clientReplyContext::traceReply(clientStreamNode * node)
{
    clientStreamNode *nextNode = (clientStreamNode *)node->node.next->data;
    StoreIOBuffer localTempBuffer;
    createStoreEntry(http->request->method, RequestFlags());
    localTempBuffer.offset = nextNode->readBuffer.offset + headers_sz;
    localTempBuffer.length = nextNode->readBuffer.length;
    localTempBuffer.data = nextNode->readBuffer.data;
    storeClientCopy(sc, http->storeEntry(),
                    localTempBuffer, SendMoreData, this);
    http->storeEntry()->releaseRequest();
    http->storeEntry()->buffer();
    const HttpReplyPointer rep(new HttpReply);
    rep->setHeaders(Http::scOkay, nullptr, "text/plain", http->request->prefixLen(), 0, squid_curtime);
    http->storeEntry()->replaceHttpReply(rep);
    http->request->swapOut(http->storeEntry());
    http->storeEntry()->complete();
}

#define SENDING_BODY 0
#define SENDING_HDRSONLY 1
int
clientReplyContext::checkTransferDone()
{
    StoreEntry *entry = http->storeEntry();

    if (entry == nullptr)
        return 0;

    /*
     * For now, 'done_copying' is used for special cases like
     * Range and HEAD requests.
     */
    if (http->flags.done_copying)
        return 1;

    if (http->request->flags.chunkedReply && !flags.complete) {
        // last-chunk was not sent
        return 0;
    }

    /*
     * Handle STORE_OK objects.
     * objectLen(entry) will be set proprely.
     * RC: Does objectLen(entry) include the Headers?
     * RC: Yes.
     */
    if (entry->store_status == STORE_OK) {
        return storeOKTransferDone();
    } else {
        return storeNotOKTransferDone();
    }
}

int
clientReplyContext::storeOKTransferDone() const
{
    assert(http->storeEntry()->objectLen() >= 0);
    assert(http->storeEntry()->objectLen() >= headers_sz);
    if (http->out.offset >= http->storeEntry()->objectLen() - headers_sz) {
        debugs(88,3, "storeOKTransferDone " <<
               " out.offset=" << http->out.offset <<
               " objectLen()=" << http->storeEntry()->objectLen() <<
               " headers_sz=" << headers_sz);
        return 1;
    }

    return 0;
}

int
clientReplyContext::storeNotOKTransferDone() const
{
    /*
     * Now, handle STORE_PENDING objects
     */
    MemObject *mem = http->storeEntry()->mem_obj;
    assert(mem != nullptr);
    assert(http->request != nullptr);

    /* mem->reply was wrong because it uses the UPSTREAM header length!!! */
    if (headers_sz == 0)
        /* haven't found end of headers yet */
        return 0;

    // TODO: Use MemObject::expectedReplySize(method) after resolving XXX below.
    const auto expectedBodySize = mem->baseReply().content_length;

    // XXX: The code below talks about sending data, and checks stats about
    // bytes written to the client connection, but this method must determine
    // whether we are done _receiving_ data from Store. This code should work OK
    // when expectedBodySize is unknown or matches written data, but it may
    // malfunction when we are writing ranges while receiving a full response.

    /*
     * Figure out how much data we are supposed to send.
     * If we are sending a body and we don't have a content-length,
     * then we must wait for the object to become STORE_OK.
     */
    if (expectedBodySize < 0)
        return 0;

    const uint64_t expectedLength = expectedBodySize + http->out.headers_sz;

    if (http->out.size < expectedLength)
        return 0;
    else {
        debugs(88,3, "storeNotOKTransferDone " <<
               " out.size=" << http->out.size <<
               " expectedLength=" << expectedLength);
        return 1;
    }
}

/* Preconditions:
 * *http is a valid structure.
 * fd is either -1, or an open fd.
 *
 * TODO: enumify this
 *
 * This function is used by any http request sink, to determine the status
 * of the object.
 */
clientStream_status_t
clientReplyStatus(clientStreamNode * aNode, ClientHttpRequest * http)
{
    clientReplyContext *context = dynamic_cast<clientReplyContext *>(aNode->data.getRaw());
    assert (context);
    assert (context->http == http);
    return context->replyStatus();
}

clientStream_status_t
clientReplyContext::replyStatus()
{
    int done;
    /* Here because lower nodes don't need it */

    if (http->storeEntry() == nullptr) {
        debugs(88, 5, "clientReplyStatus: no storeEntry");
        return STREAM_FAILED;   /* yuck, but what can we do? */
    }

    if (EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) {
        /* TODO: Could upstream read errors (result.flags.error) be
         * lost, and result in undersize requests being considered
         * complete. Should we tcp reset such connections ?
         */
        debugs(88, 5, "clientReplyStatus: aborted storeEntry");
        return STREAM_FAILED;
    }

    if ((done = checkTransferDone()) != 0 || flags.complete) {
        debugs(88, 5, "clientReplyStatus: transfer is DONE: " << done << flags.complete);
        /* Ok we're finished, but how? */

        if (EBIT_TEST(http->storeEntry()->flags, ENTRY_BAD_LENGTH)) {
            debugs(88, 5, "clientReplyStatus: truncated response body");
            return STREAM_UNPLANNED_COMPLETE;
        }

        if (!done) {
            debugs(88, 5, "clientReplyStatus: closing, !done, but read 0 bytes");
            return STREAM_FAILED;
        }

        // TODO: See also (and unify with) storeNotOKTransferDone() checks.
        const int64_t expectedBodySize =
            http->storeEntry()->mem().baseReply().bodySize(http->request->method);
        if (expectedBodySize >= 0 && !http->gotEnough()) {
            debugs(88, 5, "clientReplyStatus: client didn't get all it expected");
            return STREAM_UNPLANNED_COMPLETE;
        }

        debugs(88, 5, "clientReplyStatus: stream complete; keepalive=" <<
               http->request->flags.proxyKeepalive);
        return STREAM_COMPLETE;
    }

    // XXX: Should this be checked earlier? We could return above w/o checking.
    if (reply->receivedBodyTooLarge(*http->request, http->out.offset - 4096)) {
        /* 4096 is a margin for the HTTP headers included in out.offset */
        debugs(88, 5, "clientReplyStatus: client reply body is too large");
        return STREAM_FAILED;
    }

    return STREAM_NONE;
}

/* Responses with no body will not have a content-type header,
 * which breaks the rep_mime_type acl, which
 * coincidentally, is the most common acl for reply access lists.
 * A better long term fix for this is to allow acl matches on the various
 * status codes, and then supply a default ruleset that puts these
 * codes before any user defines access entries. That way the user
 * can choose to block these responses where appropriate, but won't get
 * mysterious breakages.
 */
bool
clientReplyContext::alwaysAllowResponse(Http::StatusCode sline) const
{
    bool result;

    switch (sline) {

    case Http::scContinue:

    case Http::scSwitchingProtocols:

    case Http::scProcessing:

    case Http::scNoContent:

    case Http::scNotModified:
        result = true;
        break;

    default:
        result = false;
    }

    return result;
}

/**
 * Generate the reply headers sent to client.
 *
 * Filters out unwanted entries and hop-by-hop from original reply header
 * then adds extra entries if we have more info than origin server
 * then adds Squid specific entries
 */
void
clientReplyContext::buildReplyHeader()
{
    HttpHeader *hdr = &reply->header;
    const bool is_hit = http->loggingTags().isTcpHit();
    HttpRequest *request = http->request;

    if (is_hit || collapsedRevalidation == crSlave)
        hdr->delById(Http::HdrType::SET_COOKIE);
    // TODO: RFC 2965 : Must honour Cache-Control: no-cache="set-cookie2" and remove header.

    // if there is not configured a peer proxy with login=PASS or login=PASSTHRU option enabled
    // remove the Proxy-Authenticate header
    if ( !request->peer_login || (strcmp(request->peer_login,"PASS") != 0 && strcmp(request->peer_login,"PASSTHRU") != 0)) {
#if USE_ADAPTATION
        // but allow adaptation services to authenticate clients
        // via request satisfaction
        if (!http->requestSatisfactionMode())
#endif
            reply->header.delById(Http::HdrType::PROXY_AUTHENTICATE);
    }

    reply->header.removeHopByHopEntries();
    // paranoid: ContentLengthInterpreter has cleaned non-generated replies
    reply->removeIrrelevantContentLength();

    //    if (request->range)
    //      clientBuildRangeHeader(http, reply);

    /*
     * Add a estimated Age header on cache hits.
     */
    if (is_hit) {
        /*
         * Remove any existing Age header sent by upstream caches
         * (note that the existing header is passed along unmodified
         * on cache misses)
         */
        hdr->delById(Http::HdrType::AGE);
        /*
         * This adds the calculated object age. Note that the details of the
         * age calculation is performed by adjusting the timestamp in
         * StoreEntry::timestampsSet(), not here.
         */
        if (EBIT_TEST(http->storeEntry()->flags, ENTRY_SPECIAL)) {
            hdr->delById(Http::HdrType::DATE);
            hdr->putTime(Http::HdrType::DATE, squid_curtime);
        } else if (http->getConn() && http->getConn()->port->actAsOrigin) {
            // Swap the Date: header to current time if we are simulating an origin
            HttpHeaderEntry *h = hdr->findEntry(Http::HdrType::DATE);
            if (h)
                hdr->putExt("X-Origin-Date", h->value.termedBuf());
            hdr->delById(Http::HdrType::DATE);
            hdr->putTime(Http::HdrType::DATE, squid_curtime);
            h = hdr->findEntry(Http::HdrType::EXPIRES);
            if (h && http->storeEntry()->expires >= 0) {
                hdr->putExt("X-Origin-Expires", h->value.termedBuf());
                hdr->delById(Http::HdrType::EXPIRES);
                hdr->putTime(Http::HdrType::EXPIRES, squid_curtime + http->storeEntry()->expires - http->storeEntry()->timestamp);
            }
            if (http->storeEntry()->timestamp <= squid_curtime) {
                // put X-Cache-Age: instead of Age:
                char age[64];
                snprintf(age, sizeof(age), "%" PRId64, static_cast<int64_t>(squid_curtime - http->storeEntry()->timestamp));
                hdr->putExt("X-Cache-Age", age);
            }
        } else if (http->storeEntry()->timestamp <= squid_curtime) {
            hdr->putInt(Http::HdrType::AGE,
                        squid_curtime - http->storeEntry()->timestamp);
        }
    }

    /* RFC 2616: Section 14.18
     *
     * Add a Date: header if missing.
     * We have access to a clock therefore are required to amend any shortcoming in servers.
     *
     * NP: done after Age: to prevent ENTRY_SPECIAL double-handling this header.
     */
    if ( !hdr->has(Http::HdrType::DATE) ) {
        if (!http->storeEntry())
            hdr->putTime(Http::HdrType::DATE, squid_curtime);
        else if (http->storeEntry()->timestamp > 0)
            hdr->putTime(Http::HdrType::DATE, http->storeEntry()->timestamp);
        else {
            debugs(88, DBG_IMPORTANT, "ERROR: Squid BUG #3279: HTTP reply without Date:");
            /* dump something useful about the problem */
            http->storeEntry()->dump(DBG_IMPORTANT);
        }
    }

    /* Filter unproxyable authentication types */
    if (http->loggingTags().oldType != LOG_TCP_DENIED &&
            hdr->has(Http::HdrType::WWW_AUTHENTICATE)) {
        HttpHeaderPos pos = HttpHeaderInitPos;
        HttpHeaderEntry *e;

        int connection_auth_blocked = 0;
        while ((e = hdr->getEntry(&pos))) {
            if (e->id == Http::HdrType::WWW_AUTHENTICATE) {
                const char *value = e->value.rawBuf();

                if ((strncasecmp(value, "NTLM", 4) == 0 &&
                        (value[4] == '\0' || value[4] == ' '))
                        ||
                        (strncasecmp(value, "Negotiate", 9) == 0 &&
                         (value[9] == '\0' || value[9] == ' '))
                        ||
                        (strncasecmp(value, "Kerberos", 8) == 0 &&
                         (value[8] == '\0' || value[8] == ' '))) {
                    if (request->flags.connectionAuthDisabled) {
                        hdr->delAt(pos, connection_auth_blocked);
                        continue;
                    }
                    request->flags.mustKeepalive = true;
                    if (!request->flags.accelerated && !request->flags.intercepted) {
                        httpHeaderPutStrf(hdr, Http::HdrType::PROXY_SUPPORT, "Session-Based-Authentication");
                        /*
                          We send "Connection: Proxy-Support" header to mark
                          Proxy-Support as a hop-by-hop header for intermediaries that do not
                          understand the semantics of this header. The RFC should have included
                          this recommendation.
                        */
                        httpHeaderPutStrf(hdr, Http::HdrType::CONNECTION, "Proxy-support");
                    }
                    break;
                }
            }
        }

        if (connection_auth_blocked)
            hdr->refreshMask();
    }

#if USE_AUTH
    /* Handle authentication headers */
    if (http->loggingTags().oldType == LOG_TCP_DENIED &&
            ( reply->sline.status() == Http::scProxyAuthenticationRequired ||
              reply->sline.status() == Http::scUnauthorized)
       ) {
        /* Add authentication header */
        /* TODO: alter errorstate to be accel on|off aware. The 0 on the next line
         * depends on authenticate behaviour: all schemes to date send no extra
         * data on 407/401 responses, and do not check the accel state on 401/407
         * responses
         */
        Auth::UserRequest::AddReplyAuthHeader(reply, request->auth_user_request, request, 0, 1);
    } else if (request->auth_user_request != nullptr)
        Auth::UserRequest::AddReplyAuthHeader(reply, request->auth_user_request, request, http->flags.accel, 0);
#endif

    SBuf cacheStatus(uniqueHostname());
    if (const auto hitOrFwd = http->loggingTags().cacheStatusSource())
        cacheStatus.append(hitOrFwd);
    if (firstStoreLookup_) {
        cacheStatus.append(";detail=");
        cacheStatus.append(firstStoreLookup_);
    }
    // TODO: Remove c_str() after converting HttpHeaderEntry::value to SBuf
    hdr->putStr(Http::HdrType::CACHE_STATUS, cacheStatus.c_str());

    const bool maySendChunkedReply = !request->multipartRangeRequest() &&
                                     reply->sline.version.protocol == AnyP::PROTO_HTTP && // response is HTTP
                                     (request->http_ver >= Http::ProtocolVersion(1,1));

    /* Check whether we should send keep-alive */
    if (!Config.onoff.error_pconns && reply->sline.status() >= 400 && !request->flags.mustKeepalive) {
        debugs(33, 3, "clientBuildReplyHeader: Error, don't keep-alive");
        request->flags.proxyKeepalive = false;
    } else if (!Config.onoff.client_pconns && !request->flags.mustKeepalive) {
        debugs(33, 2, "clientBuildReplyHeader: Connection Keep-Alive not requested by admin or client");
        request->flags.proxyKeepalive = false;
    } else if (request->flags.proxyKeepalive && shutting_down) {
        debugs(88, 3, "clientBuildReplyHeader: Shutting down, don't keep-alive.");
        request->flags.proxyKeepalive = false;
    } else if (request->flags.connectionAuth && !reply->keep_alive) {
        debugs(33, 2, "clientBuildReplyHeader: Connection oriented auth but server side non-persistent");
        request->flags.proxyKeepalive = false;
    } else if (reply->bodySize(request->method) < 0 && !maySendChunkedReply) {
        debugs(88, 3, "clientBuildReplyHeader: can't keep-alive, unknown body size" );
        request->flags.proxyKeepalive = false;
    } else if (fdUsageHigh()&& !request->flags.mustKeepalive) {
        debugs(88, 3, "clientBuildReplyHeader: Not many unused FDs, can't keep-alive");
        request->flags.proxyKeepalive = false;
    } else if (request->flags.sslBumped && !reply->persistent()) {
        // We do not really have to close, but we pretend we are a tunnel.
        debugs(88, 3, "clientBuildReplyHeader: bumped reply forces close");
        request->flags.proxyKeepalive = false;
    } else if (request->pinnedConnection() && !reply->persistent()) {
        // The peer wants to close the pinned connection
        debugs(88, 3, "pinned reply forces close");
        request->flags.proxyKeepalive = false;
    } else if (http->getConn()) {
        ConnStateData * conn = http->getConn();
        if (!Comm::IsConnOpen(conn->port->listenConn)) {
            // The listening port closed because of a reconfigure
            debugs(88, 3, "listening port closed");
            request->flags.proxyKeepalive = false;
        }
    }

    // Decide if we send chunked reply
    if (maySendChunkedReply && reply->bodySize(request->method) < 0) {
        debugs(88, 3, "clientBuildReplyHeader: chunked reply");
        request->flags.chunkedReply = true;
        hdr->putStr(Http::HdrType::TRANSFER_ENCODING, "chunked");
    }

    hdr->addVia(reply->sline.version);

    /* Signal keep-alive or close explicitly */
    hdr->putStr(Http::HdrType::CONNECTION, request->flags.proxyKeepalive ? "keep-alive" : "close");

#if ADD_X_REQUEST_URI
    /*
     * Knowing the URI of the request is useful when debugging persistent
     * connections in a client; we cannot guarantee the order of http headers,
     * but X-Request-URI is likely to be the very last header to ease use from a
     * debugger [hdr->entries.count-1].
     */
    hdr->putStr(Http::HdrType::X_REQUEST_URI,
                http->memOjbect()->url ? http->memObject()->url : http->uri);

#endif

    /* Surrogate-Control requires Surrogate-Capability from upstream to pass on */
    if ( hdr->has(Http::HdrType::SURROGATE_CONTROL) ) {
        if (!request->header.has(Http::HdrType::SURROGATE_CAPABILITY)) {
            hdr->delById(Http::HdrType::SURROGATE_CONTROL);
        }
        /* TODO: else case: drop any controls intended specifically for our surrogate ID */
    }

    httpHdrMangleList(hdr, request, http->al, ROR_REPLY);
}

void
clientReplyContext::cloneReply()
{
    assert(reply == nullptr);

    reply = http->storeEntry()->mem().freshestReply().clone();
    HTTPMSGLOCK(reply);

    http->al->reply = reply;

    if (reply->sline.version.protocol == AnyP::PROTO_HTTP) {
        /* RFC 2616 requires us to advertise our version (but only on real HTTP traffic) */
        reply->sline.version = Http::ProtocolVersion();
    }

    /* do header conversions */
    buildReplyHeader();
}

/// Safely disposes of an entry pointing to a cache hit that we do not want.
/// We cannot just ignore the entry because it may be locking or otherwise
/// holding an associated cache resource of some sort.
void
clientReplyContext::forgetHit()
{
    StoreEntry *e = http->storeEntry();
    assert(e); // or we are not dealing with a hit
    // We probably have not locked the entry earlier, unfortunately. We lock it
    // now so that we can unlock two lines later (and trigger cleanup).
    // Ideally, ClientHttpRequest::storeEntry() should lock/unlock, but it is
    // used so inconsistently that simply adding locking there leads to bugs.
    e->lock("clientReplyContext::forgetHit");
    http->storeEntry(nullptr);
    e->unlock("clientReplyContext::forgetHit"); // may delete e
}

void
clientReplyContext::identifyStoreObject()
{
    HttpRequest *r = http->request;

    // client sent CC:no-cache or some other condition has been
    // encountered which prevents delivering a public/cached object.
    if (!r->flags.noCache || r->flags.internal) {
        const auto e = storeGetPublicByRequest(r);
        identifyFoundObject(e, storeLookupString(bool(e)));
    } else {
        // "external" no-cache requests skip Store lookups
        identifyFoundObject(nullptr, "no-cache");
    }
}

/**
 * Check state of the current StoreEntry object.
 * to see if we can determine the final status of the request.
 */
void
clientReplyContext::identifyFoundObject(StoreEntry *newEntry, const char *detail)
{
    detailStoreLookup(detail);

    HttpRequest *r = http->request;
    http->storeEntry(newEntry);
    const auto e = http->storeEntry();

    /* Release IP-cache entries on reload */
    /** \li If the request has no-cache flag set or some no_cache HACK in operation we
      * 'invalidate' the cached IP entries for this request ???
      */
    if (r->flags.noCache || r->flags.noCacheHack())
        ipcacheInvalidateNegative(r->url.host());

    if (!e) {
        /** \li If no StoreEntry object is current assume this object isn't in the cache set MISS*/
        debugs(85, 3, "StoreEntry is NULL -  MISS");
        http->updateLoggingTags(LOG_TCP_MISS);
        doGetMoreData();
        return;
    }

    if (Config.onoff.offline) {
        /** \li If we are running in offline mode set to HIT */
        debugs(85, 3, "offline HIT " << *e);
        http->updateLoggingTags(LOG_TCP_HIT);
        doGetMoreData();
        return;
    }

    if (http->redirect.status) {
        /** \li If redirection status is True force this to be a MISS */
        debugs(85, 3, "REDIRECT status forced StoreEntry to NULL (no body on 3XX responses) " << *e);
        forgetHit();
        http->updateLoggingTags(LOG_TCP_REDIRECT);
        doGetMoreData();
        return;
    }

    if (!e->validToSend()) {
        debugs(85, 3, "!storeEntryValidToSend MISS " << *e);
        forgetHit();
        http->updateLoggingTags(LOG_TCP_MISS);
        doGetMoreData();
        return;
    }

    if (EBIT_TEST(e->flags, ENTRY_SPECIAL)) {
        /* \li Special entries are always hits, no matter what the client says */
        debugs(85, 3, "ENTRY_SPECIAL HIT " << *e);
        http->updateLoggingTags(LOG_TCP_HIT);
        doGetMoreData();
        return;
    }

    if (r->flags.noCache) {
        debugs(85, 3, "no-cache REFRESH MISS " << *e);
        forgetHit();
        http->updateLoggingTags(LOG_TCP_CLIENT_REFRESH_MISS);
        doGetMoreData();
        return;
    }

    if (e->hittingRequiresCollapsing() && !startCollapsingOn(*e, false)) {
        debugs(85, 3, "prohibited CF MISS " << *e);
        forgetHit();
        http->updateLoggingTags(LOG_TCP_MISS);
        doGetMoreData();
        return;
    }

    debugs(85, 3, "default HIT " << *e);
    http->updateLoggingTags(LOG_TCP_HIT);
    doGetMoreData();
}

/// remembers the very first Store lookup classification, ignoring the rest
void
clientReplyContext::detailStoreLookup(const char *detail)
{
    if (!firstStoreLookup_) {
        debugs(85, 7, detail);
        firstStoreLookup_ = detail;
    } else {
        debugs(85, 7, "ignores " << detail << " after " << firstStoreLookup_);
    }
}

/**
 * Request more data from the store for the client Stream
 * This is *the* entry point to this module.
 *
 * Preconditions:
 *  - This is the head of the list.
 *  - There is at least one more node.
 *  - Data context is not null
 */
void
clientGetMoreData(clientStreamNode * aNode, ClientHttpRequest * http)
{
    /* Test preconditions */
    assert(aNode != nullptr);
    assert(cbdataReferenceValid(aNode));
    assert(aNode->node.prev == nullptr);
    assert(aNode->node.next != nullptr);
    clientReplyContext *context = dynamic_cast<clientReplyContext *>(aNode->data.getRaw());
    assert (context);
    assert(context->http == http);

    clientStreamNode *next = ( clientStreamNode *)aNode->node.next->data;

    if (!context->ourNode)
        context->ourNode = aNode;

    /* no cbdatareference, this is only used once, and safely */
    if (context->flags.storelogiccomplete) {
        StoreIOBuffer tempBuffer;
        tempBuffer.offset = next->readBuffer.offset + context->headers_sz;
        tempBuffer.length = next->readBuffer.length;
        tempBuffer.data = next->readBuffer.data;

        storeClientCopy(context->sc, http->storeEntry(),
                        tempBuffer, clientReplyContext::SendMoreData, context);
        return;
    }

    if (context->http->request->method == Http::METHOD_PURGE) {
        context->purgeRequest();
        return;
    }

    // OPTIONS with Max-Forwards:0 handled in clientProcessRequest()

    if (context->http->request->method == Http::METHOD_TRACE) {
        if (context->http->request->header.getInt64(Http::HdrType::MAX_FORWARDS) == 0) {
            context->traceReply(aNode);
            return;
        }

        /* continue forwarding, not finished yet. */
        http->updateLoggingTags(LOG_TCP_MISS);

        context->doGetMoreData();
    } else
        context->identifyStoreObject();
}

void
clientReplyContext::doGetMoreData()
{
    /* We still have to do store logic processing - vary, cache hit etc */
    if (http->storeEntry() != nullptr) {
        /* someone found the object in the cache for us */
        StoreIOBuffer localTempBuffer;

        http->storeEntry()->lock("clientReplyContext::doGetMoreData");

        http->storeEntry()->ensureMemObject(storeId(), http->log_uri, http->request->method);

        sc = storeClientListAdd(http->storeEntry(), this);
#if USE_DELAY_POOLS
        sc->setDelayId(DelayId::DelayClient(http));
#endif

        assert(http->loggingTags().oldType == LOG_TCP_HIT);
        reqofs = 0;
        /* guarantee nothing has been sent yet! */
        assert(http->out.size == 0);
        assert(http->out.offset == 0);

        if (ConnStateData *conn = http->getConn()) {
            if (Ip::Qos::TheConfig.isHitTosActive()) {
                Ip::Qos::doTosLocalHit(conn->clientConnection);
            }

            if (Ip::Qos::TheConfig.isHitNfmarkActive()) {
                Ip::Qos::doNfmarkLocalHit(conn->clientConnection);
            }
        }

        localTempBuffer.offset = reqofs;
        localTempBuffer.length = getNextNode()->readBuffer.length;
        localTempBuffer.data = getNextNode()->readBuffer.data;
        storeClientCopy(sc, http->storeEntry(), localTempBuffer, CacheHit, this);
    } else {
        /* MISS CASE, http->loggingTags() are already set! */
        processMiss();
    }
}

/** The next node has removed itself from the stream. */
void
clientReplyDetach(clientStreamNode * node, ClientHttpRequest * http)
{
    /** detach from the stream */
    clientStreamDetach(node, http);
}

/**
 * Accepts chunk of a http message in buf, parses prefix, filters headers and
 * such, writes processed message to the message recipient
 */
void
clientReplyContext::SendMoreData(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = static_cast<clientReplyContext *>(data);
    context->sendMoreData (result);
}

void
clientReplyContext::makeThisHead()
{
    /* At least, I think that's what this does */
    dlinkDelete(&http->active, &ClientActiveRequests);
    dlinkAdd(http, &http->active, &ClientActiveRequests);
}

bool
clientReplyContext::errorInStream(StoreIOBuffer const &result, size_t const &sizeToProcess)const
{
    return /* aborted request */
        (http->storeEntry() && EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) ||
        /* Upstream read error */ (result.flags.error) ||
        /* Upstream EOF */ (sizeToProcess == 0);
}

void
clientReplyContext::sendStreamError(StoreIOBuffer const &result)
{
    /** call clientWriteComplete so the client socket gets closed
     *
     * We call into the stream, because we don't know that there is a
     * client socket!
     */
    debugs(88, 5, "A stream error has occurred, marking as complete and sending no data.");
    StoreIOBuffer localTempBuffer;
    flags.complete = 1;
    http->request->flags.streamError = true;
    localTempBuffer.flags.error = result.flags.error;
    clientStreamCallback((clientStreamNode*)http->client_stream.head->data, http, nullptr,
                         localTempBuffer);
}

void
clientReplyContext::pushStreamData(StoreIOBuffer const &result, char *source)
{
    StoreIOBuffer localTempBuffer;

    if (result.length == 0) {
        debugs(88, 5, "clientReplyContext::pushStreamData: marking request as complete due to 0 length store result");
        flags.complete = 1;
    }

    assert(result.offset - headers_sz == next()->readBuffer.offset);
    localTempBuffer.offset = result.offset - headers_sz;
    localTempBuffer.length = result.length;

    if (localTempBuffer.length)
        localTempBuffer.data = source;

    clientStreamCallback((clientStreamNode*)http->client_stream.head->data, http, nullptr,
                         localTempBuffer);
}

clientStreamNode *
clientReplyContext::next() const
{
    assert ( (clientStreamNode*)http->client_stream.head->next->data == getNextNode());
    return getNextNode();
}

void
clientReplyContext::sendBodyTooLargeError()
{
    http->updateLoggingTags(LOG_TCP_DENIED_REPLY);
    ErrorState *err = clientBuildError(ERR_TOO_BIG, Http::scForbidden, nullptr,
                                       http->getConn(), http->request, http->al);
    removeClientStoreReference(&(sc), http);
    HTTPMSGUNLOCK(reply);
    startError(err);

}

/// send 412 (Precondition Failed) to client
void
clientReplyContext::sendPreconditionFailedError()
{
    http->updateLoggingTags(LOG_TCP_HIT);
    ErrorState *const err =
        clientBuildError(ERR_PRECONDITION_FAILED, Http::scPreconditionFailed,
                         nullptr, http->getConn(), http->request, http->al);
    removeClientStoreReference(&sc, http);
    HTTPMSGUNLOCK(reply);
    startError(err);
}

/// send 304 (Not Modified) to client
void
clientReplyContext::sendNotModified()
{
    StoreEntry *e = http->storeEntry();
    const time_t timestamp = e->timestamp;
    const auto temprep = e->mem().freshestReply().make304();
    // log as TCP_INM_HIT if code 304 generated for
    // If-None-Match request
    if (!http->request->flags.ims)
        http->updateLoggingTags(LOG_TCP_INM_HIT);
    else
        http->updateLoggingTags(LOG_TCP_IMS_HIT);
    removeClientStoreReference(&sc, http);
    createStoreEntry(http->request->method, RequestFlags());
    e = http->storeEntry();
    // Copy timestamp from the original entry so the 304
    // reply has a meaningful Age: header.
    e->timestampsSet();
    e->timestamp = timestamp;
    e->replaceHttpReply(temprep);
    e->complete();
    /*
     * TODO: why put this in the store and then serialise it and
     * then parse it again. Simply mark the request complete in
     * our context and write the reply struct to the client side.
     */
    triggerInitialStoreRead();
}

/// send 304 (Not Modified) or 412 (Precondition Failed) to client
/// depending on request method
void
clientReplyContext::sendNotModifiedOrPreconditionFailedError()
{
    if (http->request->method == Http::METHOD_GET ||
            http->request->method == Http::METHOD_HEAD)
        sendNotModified();
    else
        sendPreconditionFailedError();
}

void
clientReplyContext::processReplyAccess ()
{
    /* NP: this should probably soft-fail to a zero-sized-reply error ?? */
    assert(reply);

    /** Don't block our own responses or HTTP status messages */
    if (http->loggingTags().oldType == LOG_TCP_DENIED ||
            http->loggingTags().oldType == LOG_TCP_DENIED_REPLY ||
            alwaysAllowResponse(reply->sline.status())) {
        headers_sz = reply->hdr_sz;
        processReplyAccessResult(ACCESS_ALLOWED);
        return;
    }

    /** Check for reply to big error */
    if (reply->expectedBodyTooLarge(*http->request)) {
        sendBodyTooLargeError();
        return;
    }

    headers_sz = reply->hdr_sz;

    /** check for absent access controls (permit by default) */
    if (!Config.accessList.reply) {
        processReplyAccessResult(ACCESS_ALLOWED);
        return;
    }

    /** Process http_reply_access lists */
    ACLFilledChecklist *replyChecklist =
        clientAclChecklistCreate(Config.accessList.reply, http);
    replyChecklist->reply = reply;
    HTTPMSGLOCK(replyChecklist->reply);
    replyChecklist->nonBlockingCheck(ProcessReplyAccessResult, this);
}

void
clientReplyContext::ProcessReplyAccessResult(Acl::Answer rv, void *voidMe)
{
    clientReplyContext *me = static_cast<clientReplyContext *>(voidMe);
    me->processReplyAccessResult(rv);
}

void
clientReplyContext::processReplyAccessResult(const Acl::Answer &accessAllowed)
{
    debugs(88, 2, "The reply for " << http->request->method
           << ' ' << http->uri << " is " << accessAllowed << ", because it matched "
           << (AclMatchedName ? AclMatchedName : "NO ACL's"));

    if (!accessAllowed.allowed()) {
        ErrorState *err;
        err_type page_id;
        page_id = aclGetDenyInfoPage(&Config.denyInfoList, AclMatchedName, 1);

        http->updateLoggingTags(LOG_TCP_DENIED_REPLY);

        if (page_id == ERR_NONE)
            page_id = ERR_ACCESS_DENIED;

        err = clientBuildError(page_id, Http::scForbidden, nullptr,
                               http->getConn(), http->request, http->al);

        removeClientStoreReference(&sc, http);

        HTTPMSGUNLOCK(reply);

        startError(err);

        return;
    }

    /* Ok, the reply is allowed, */
    http->loggingEntry(http->storeEntry());

    ssize_t body_size = reqofs - reply->hdr_sz;
    if (body_size < 0) {
        reqofs = reply->hdr_sz;
        body_size = 0;
    }

    debugs(88, 3, "clientReplyContext::sendMoreData: Appending " <<
           (int) body_size << " bytes after " << reply->hdr_sz <<
           " bytes of headers");

#if USE_SQUID_ESI

    if (http->flags.accel && reply->sline.status() != Http::scForbidden &&
            !alwaysAllowResponse(reply->sline.status()) &&
            esiEnableProcessing(reply)) {
        debugs(88, 2, "Enabling ESI processing for " << http->uri);
        clientStreamInsertHead(&http->client_stream, esiStreamRead,
                               esiProcessStream, esiStreamDetach, esiStreamStatus, nullptr);
    }

#endif

    if (http->request->method == Http::METHOD_HEAD) {
        /* do not forward body for HEAD replies */
        body_size = 0;
        http->flags.done_copying = true;
        flags.complete = 1;
    }

    assert (!flags.headersSent);
    flags.headersSent = true;

    StoreIOBuffer localTempBuffer;
    char *buf = next()->readBuffer.data;
    char *body_buf = buf + reply->hdr_sz;

    //Server side may disable ranges under some circumstances.

    if ((!http->request->range))
        next()->readBuffer.offset = 0;

    body_buf -= next()->readBuffer.offset;

    if (next()->readBuffer.offset != 0) {
        if (next()->readBuffer.offset > body_size) {
            /* Can't use any of the body we received. send nothing */
            localTempBuffer.length = 0;
            localTempBuffer.data = nullptr;
        } else {
            localTempBuffer.length = body_size - next()->readBuffer.offset;
            localTempBuffer.data = body_buf + next()->readBuffer.offset;
        }
    } else {
        localTempBuffer.length = body_size;
        localTempBuffer.data = body_buf;
    }

    /* TODO??: move the data in the buffer back by the request header size */
    clientStreamCallback((clientStreamNode *)http->client_stream.head->data,
                         http, reply, localTempBuffer);

    return;
}

void
clientReplyContext::sendMoreData (StoreIOBuffer result)
{
    if (deleting)
        return;

    StoreEntry *entry = http->storeEntry();

    if (ConnStateData * conn = http->getConn()) {
        if (!conn->isOpen()) {
            debugs(33,3, "not sending more data to closing connection " << conn->clientConnection);
            return;
        }
        if (conn->pinning.zeroReply) {
            debugs(33,3, "not sending more data after a pinned zero reply " << conn->clientConnection);
            return;
        }

        if (reqofs==0 && !http->loggingTags().isTcpHit()) {
            if (Ip::Qos::TheConfig.isHitTosActive()) {
                Ip::Qos::doTosLocalMiss(conn->clientConnection, http->request->hier.code);
            }
            if (Ip::Qos::TheConfig.isHitNfmarkActive()) {
                Ip::Qos::doNfmarkLocalMiss(conn->clientConnection, http->request->hier.code);
            }
        }

        debugs(88, 5, conn->clientConnection <<
               " '" << entry->url() << "'" <<
               " out.offset=" << http->out.offset);
    }

    char *buf = next()->readBuffer.data;

    if (buf != result.data) {
        /* we've got to copy some data */
        assert(result.length <= next()->readBuffer.length);
        memcpy(buf, result.data, result.length);
    }

    /* We've got the final data to start pushing... */
    flags.storelogiccomplete = 1;

    reqofs += result.length;

    assert(reqofs <= HTTP_REQBUF_SZ || flags.headersSent);

    assert(http->request != nullptr);

    /* ESI TODO: remove this assert once everything is stable */
    assert(http->client_stream.head->data
           && cbdataReferenceValid(http->client_stream.head->data));

    makeThisHead();

    debugs(88, 5, "clientReplyContext::sendMoreData: " << http->uri << ", " <<
           reqofs << " bytes (" << result.length <<
           " new bytes)");

    /* update size of the request */
    reqsize = reqofs;

    if (errorInStream(result, reqofs)) {
        sendStreamError(result);
        return;
    }

    if (flags.headersSent) {
        pushStreamData (result, buf);
        return;
    }

    cloneReply();

#if USE_DELAY_POOLS
    if (sc)
        sc->setDelayId(DelayId::DelayClient(http,reply));
#endif

    processReplyAccess();
    return;
}

void
clientReplyContext::fillChecklist(ACLFilledChecklist &checklist) const
{
    clientAclChecklistFill(checklist, http);
}

/* Using this breaks the client layering just a little!
 */
void
clientReplyContext::createStoreEntry(const HttpRequestMethod& m, RequestFlags reqFlags)
{
    assert(http != nullptr);
    /*
     * For erroneous requests, we might not have a h->request,
     * so make a fake one.
     */

    if (http->request == nullptr) {
        const auto connManager = http->getConn();
        const auto mx = MasterXaction::MakePortful(connManager ? connManager->port : nullptr);
        // XXX: These fake URI parameters shadow the real (or error:...) URI.
        // TODO: Either always set the request earlier and assert here OR use
        // http->uri (converted to Anyp::Uri) to create this catch-all request.
        const_cast<HttpRequest *&>(http->request) =  new HttpRequest(m, AnyP::PROTO_NONE, "http", null_string, mx);
        HTTPMSGLOCK(http->request);
    }

    StoreEntry *e = storeCreateEntry(storeId(), http->log_uri, reqFlags, m);

    // Make entry collapsible ASAP, to increase collapsing chances for others,
    // TODO: every must-revalidate and similar request MUST reach the origin,
    // but do we have to prohibit others from collapsing on that request?
    if (reqFlags.cachable &&
            !reqFlags.needValidation &&
            (m == Http::METHOD_GET || m == Http::METHOD_HEAD) &&
            mayInitiateCollapsing()) {
        // make the entry available for future requests now
        (void)Store::Root().allowCollapsing(e, reqFlags, m);
    }

    sc = storeClientListAdd(e, this);

#if USE_DELAY_POOLS
    sc->setDelayId(DelayId::DelayClient(http));
#endif

    reqofs = 0;

    reqsize = 0;

    /* I don't think this is actually needed! -- adrian */
    /* http->reqbuf = http->norm_reqbuf; */
    //    assert(http->reqbuf == http->norm_reqbuf);
    /* The next line is illegal because we don't know if the client stream
     * buffers have been set up
     */
    //    storeClientCopy(http->sc, e, 0, HTTP_REQBUF_SZ, http->reqbuf,
    //        SendMoreData, this);
    /* So, we mark the store logic as complete */
    flags.storelogiccomplete = 1;

    /* and get the caller to request a read, from wherever they are */
    /* NOTE: after ANY data flows down the pipe, even one step,
     * this function CAN NOT be used to manage errors
     */
    http->storeEntry(e);
}

ErrorState *
clientBuildError(err_type page_id, Http::StatusCode status, char const *url,
                 const ConnStateData *conn, HttpRequest *request, const AccessLogEntry::Pointer &al)
{
    const auto err = new ErrorState(page_id, status, request, al);
    err->src_addr = conn && conn->clientConnection ? conn->clientConnection->remote : Ip::Address::NoAddr();

    if (url)
        err->url = xstrdup(url);

    return err;
}

