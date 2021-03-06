/* -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Corporation
 * Portions created by the Initial Developer are Copyright (C) 2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Dave Camp <dcamp@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsOfflineCacheUpdate.h"

#include "nsCPrefetchService.h"
#include "nsCURILoader.h"
#include "nsIApplicationCacheContainer.h"
#include "nsIApplicationCacheChannel.h"
#include "nsIApplicationCacheService.h"
#include "nsICache.h"
#include "nsICacheService.h"
#include "nsICacheSession.h"
#include "nsICachingChannel.h"
#include "nsIContent.h"
#include "mozilla/dom/Element.h"
#include "nsIDocumentLoader.h"
#include "nsIDOMElement.h"
#include "nsIDOMWindow.h"
#include "nsIDOMOfflineResourceList.h"
#include "nsIDocument.h"
#include "nsIObserverService.h"
#include "nsIURL.h"
#include "nsIWebProgress.h"
#include "nsICryptoHash.h"
#include "nsICacheEntryDescriptor.h"
#include "nsIPermissionManager.h"
#include "nsIPrincipal.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsStreamUtils.h"
#include "nsThreadUtils.h"
#include "nsProxyRelease.h"
#include "prlog.h"
#include "nsIAsyncVerifyRedirectCallback.h"

#include "nsXULAppAPI.h"

static const PRUint32 kRescheduleLimit = 3;

#if defined(PR_LOGGING)
//
// To enable logging (see prlog.h for full details):
//
//    set NSPR_LOG_MODULES=nsOfflineCacheUpdate:5
//    set NSPR_LOG_FILE=offlineupdate.log
//
// this enables PR_LOG_ALWAYS level information and places all output in
// the file offlineupdate.log
//
extern PRLogModuleInfo *gOfflineCacheUpdateLog;
#endif
#define LOG(args) PR_LOG(gOfflineCacheUpdateLog, 4, args)
#define LOG_ENABLED() PR_LOG_TEST(gOfflineCacheUpdateLog, 4)

class AutoFreeArray {
public:
    AutoFreeArray(PRUint32 count, char **values)
        : mCount(count), mValues(values) {};
    ~AutoFreeArray() { NS_FREE_XPCOM_ALLOCATED_POINTER_ARRAY(mCount, mValues); }
private:
    PRUint32 mCount;
    char **mValues;
};

static nsresult
DropReferenceFromURL(nsIURI * aURI)
{
    nsCOMPtr<nsIURL> url = do_QueryInterface(aURI);
    if (url) {
        nsresult rv = url->SetRef(EmptyCString());
        NS_ENSURE_SUCCESS(rv, rv);
    }

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsManifestCheck
//-----------------------------------------------------------------------------

class nsManifestCheck : public nsIStreamListener
                      , public nsIChannelEventSink
                      , public nsIInterfaceRequestor
{
public:
    nsManifestCheck(nsOfflineCacheUpdate *aUpdate,
                    nsIURI *aURI,
                    nsIURI *aReferrerURI)
        : mUpdate(aUpdate)
        , mURI(aURI)
        , mReferrerURI(aReferrerURI)
        {}

    NS_DECL_ISUPPORTS
    NS_DECL_NSIREQUESTOBSERVER
    NS_DECL_NSISTREAMLISTENER
    NS_DECL_NSICHANNELEVENTSINK
    NS_DECL_NSIINTERFACEREQUESTOR

    nsresult Begin();

private:

    static NS_METHOD ReadManifest(nsIInputStream *aInputStream,
                                  void *aClosure,
                                  const char *aFromSegment,
                                  PRUint32 aOffset,
                                  PRUint32 aCount,
                                  PRUint32 *aBytesConsumed);

    nsRefPtr<nsOfflineCacheUpdate> mUpdate;
    nsCOMPtr<nsIURI> mURI;
    nsCOMPtr<nsIURI> mReferrerURI;
    nsCOMPtr<nsICryptoHash> mManifestHash;
    nsCOMPtr<nsIChannel> mChannel;
};

//-----------------------------------------------------------------------------
// nsManifestCheck::nsISupports
//-----------------------------------------------------------------------------
NS_IMPL_ISUPPORTS4(nsManifestCheck,
                   nsIRequestObserver,
                   nsIStreamListener,
                   nsIChannelEventSink,
                   nsIInterfaceRequestor)

//-----------------------------------------------------------------------------
// nsManifestCheck <public>
//-----------------------------------------------------------------------------

nsresult
nsManifestCheck::Begin()
{
    nsresult rv;
    mManifestHash = do_CreateInstance("@mozilla.org/security/hash;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = mManifestHash->Init(nsICryptoHash::MD5);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_NewChannel(getter_AddRefs(mChannel),
                       mURI,
                       nsnull, nsnull, nsnull,
                       nsIRequest::LOAD_BYPASS_CACHE);
    NS_ENSURE_SUCCESS(rv, rv);

    // configure HTTP specific stuff
    nsCOMPtr<nsIHttpChannel> httpChannel =
        do_QueryInterface(mChannel);
    if (httpChannel) {
        httpChannel->SetReferrer(mReferrerURI);
        httpChannel->SetRequestHeader(NS_LITERAL_CSTRING("X-Moz"),
                                      NS_LITERAL_CSTRING("offline-resource"),
                                      PR_FALSE);
    }

    rv = mChannel->AsyncOpen(this, nsnull);
    NS_ENSURE_SUCCESS(rv, rv);

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsManifestCheck <public>
//-----------------------------------------------------------------------------

/* static */
NS_METHOD
nsManifestCheck::ReadManifest(nsIInputStream *aInputStream,
                              void *aClosure,
                              const char *aFromSegment,
                              PRUint32 aOffset,
                              PRUint32 aCount,
                              PRUint32 *aBytesConsumed)
{
    nsManifestCheck *manifestCheck =
        static_cast<nsManifestCheck*>(aClosure);

    nsresult rv;
    *aBytesConsumed = aCount;

    rv = manifestCheck->mManifestHash->Update(
        reinterpret_cast<const PRUint8 *>(aFromSegment), aCount);
    NS_ENSURE_SUCCESS(rv, rv);

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsManifestCheck::nsIStreamListener
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsManifestCheck::OnStartRequest(nsIRequest *aRequest,
                                nsISupports *aContext)
{
    return NS_OK;
}

NS_IMETHODIMP
nsManifestCheck::OnDataAvailable(nsIRequest *aRequest,
                                 nsISupports *aContext,
                                 nsIInputStream *aStream,
                                 PRUint32 aOffset,
                                 PRUint32 aCount)
{
    PRUint32 bytesRead;
    aStream->ReadSegments(ReadManifest, this, aCount, &bytesRead);
    return NS_OK;
}

NS_IMETHODIMP
nsManifestCheck::OnStopRequest(nsIRequest *aRequest,
                               nsISupports *aContext,
                               nsresult aStatus)
{
    nsCAutoString manifestHash;
    if (NS_SUCCEEDED(aStatus)) {
        mManifestHash->Finish(PR_TRUE, manifestHash);
    }

    mUpdate->ManifestCheckCompleted(aStatus, manifestHash);

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsManifestCheck::nsIInterfaceRequestor
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsManifestCheck::GetInterface(const nsIID &aIID, void **aResult)
{
    if (aIID.Equals(NS_GET_IID(nsIChannelEventSink))) {
        NS_ADDREF_THIS();
        *aResult = static_cast<nsIChannelEventSink *>(this);
        return NS_OK;
    }

    return NS_ERROR_NO_INTERFACE;
}

//-----------------------------------------------------------------------------
// nsManifestCheck::nsIChannelEventSink
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsManifestCheck::AsyncOnChannelRedirect(nsIChannel *aOldChannel,
                                        nsIChannel *aNewChannel,
                                        PRUint32 aFlags,
                                        nsIAsyncVerifyRedirectCallback *callback)
{
    // Redirects should cause the load (and therefore the update) to fail.
    if (aFlags & nsIChannelEventSink::REDIRECT_INTERNAL) {
        callback->OnRedirectVerifyCallback(NS_OK);
        return NS_OK;
    }
    aOldChannel->Cancel(NS_ERROR_ABORT);
    return NS_ERROR_ABORT;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ISUPPORTS6(nsOfflineCacheUpdateItem,
                   nsIDOMLoadStatus,
                   nsIRequestObserver,
                   nsIStreamListener,
                   nsIRunnable,
                   nsIInterfaceRequestor,
                   nsIChannelEventSink)

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem <public>
//-----------------------------------------------------------------------------

nsOfflineCacheUpdateItem::nsOfflineCacheUpdateItem(nsOfflineCacheUpdate *aUpdate,
                                                   nsIURI *aURI,
                                                   nsIURI *aReferrerURI,
                                                   nsIApplicationCache *aPreviousApplicationCache,
                                                   const nsACString &aClientID,
                                                   PRUint32 type)
    : mURI(aURI)
    , mReferrerURI(aReferrerURI)
    , mPreviousApplicationCache(aPreviousApplicationCache)
    , mClientID(aClientID)
    , mItemType(type)
    , mUpdate(aUpdate)
    , mChannel(nsnull)
    , mState(nsIDOMLoadStatus::UNINITIALIZED)
    , mBytesRead(0)
{
}

nsOfflineCacheUpdateItem::~nsOfflineCacheUpdateItem()
{
}

nsresult
nsOfflineCacheUpdateItem::OpenChannel()
{
#if defined(PR_LOGGING)
    if (LOG_ENABLED()) {
        nsCAutoString spec;
        mURI->GetSpec(spec);
        LOG(("%p: Opening channel for %s", this, spec.get()));
    }
#endif

    nsresult rv = nsOfflineCacheUpdate::GetCacheKey(mURI, mCacheKey);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_NewChannel(getter_AddRefs(mChannel),
                       mURI,
                       nsnull, nsnull, this,
                       nsIRequest::LOAD_BACKGROUND |
                       nsICachingChannel::LOAD_ONLY_IF_MODIFIED |
                       nsICachingChannel::LOAD_CHECK_OFFLINE_CACHE);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIApplicationCacheChannel> appCacheChannel =
        do_QueryInterface(mChannel, &rv);

    // Support for nsIApplicationCacheChannel is required.
    NS_ENSURE_SUCCESS(rv, rv);

    // Use the existing application cache as the cache to check.
    rv = appCacheChannel->SetApplicationCache(mPreviousApplicationCache);
    NS_ENSURE_SUCCESS(rv, rv);

    // configure HTTP specific stuff
    nsCOMPtr<nsIHttpChannel> httpChannel =
        do_QueryInterface(mChannel);
    if (httpChannel) {
        httpChannel->SetReferrer(mReferrerURI);
        httpChannel->SetRequestHeader(NS_LITERAL_CSTRING("X-Moz"),
                                      NS_LITERAL_CSTRING("offline-resource"),
                                      PR_FALSE);
    }

    nsCOMPtr<nsICachingChannel> cachingChannel =
        do_QueryInterface(mChannel);
    if (cachingChannel) {
        rv = cachingChannel->SetCacheForOfflineUse(PR_TRUE);
        NS_ENSURE_SUCCESS(rv, rv);

        if (!mClientID.IsEmpty()) {
            rv = cachingChannel->SetOfflineCacheClientID(mClientID);
            NS_ENSURE_SUCCESS(rv, rv);
        }
    }

    rv = mChannel->AsyncOpen(this, nsnull);
    NS_ENSURE_SUCCESS(rv, rv);

    mState = nsIDOMLoadStatus::REQUESTED;

    return NS_OK;
}

nsresult
nsOfflineCacheUpdateItem::Cancel()
{
    if (mChannel) {
        mChannel->Cancel(NS_ERROR_ABORT);
        mChannel = nsnull;
    }

    mState = nsIDOMLoadStatus::UNINITIALIZED;

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsIStreamListener
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsOfflineCacheUpdateItem::OnStartRequest(nsIRequest *aRequest,
                                         nsISupports *aContext)
{
    mState = nsIDOMLoadStatus::RECEIVING;

    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::OnDataAvailable(nsIRequest *aRequest,
                                          nsISupports *aContext,
                                          nsIInputStream *aStream,
                                          PRUint32 aOffset,
                                          PRUint32 aCount)
{
    PRUint32 bytesRead = 0;
    aStream->ReadSegments(NS_DiscardSegment, nsnull, aCount, &bytesRead);
    mBytesRead += bytesRead;
    LOG(("loaded %u bytes into offline cache [offset=%u]\n",
         bytesRead, aOffset));
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::OnStopRequest(nsIRequest *aRequest,
                                        nsISupports *aContext,
                                        nsresult aStatus)
{
    LOG(("done fetching offline item [status=%x]\n", aStatus));

    mState = nsIDOMLoadStatus::LOADED;

    if (mBytesRead == 0 && aStatus == NS_OK) {
        // we didn't need to read (because LOAD_ONLY_IF_MODIFIED was
        // specified), but the object should report loadedSize as if it
        // did.
        mChannel->GetContentLength(&mBytesRead);
    }

    // We need to notify the update that the load is complete, but we
    // want to give the channel a chance to close the cache entries.
    NS_DispatchToCurrentThread(this);

    return NS_OK;
}


//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsIRunnable
//-----------------------------------------------------------------------------
NS_IMETHODIMP
nsOfflineCacheUpdateItem::Run()
{
    mUpdate->LoadCompleted();

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsIInterfaceRequestor
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetInterface(const nsIID &aIID, void **aResult)
{
    if (aIID.Equals(NS_GET_IID(nsIChannelEventSink))) {
        NS_ADDREF_THIS();
        *aResult = static_cast<nsIChannelEventSink *>(this);
        return NS_OK;
    }

    return NS_ERROR_NO_INTERFACE;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsIChannelEventSink
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsOfflineCacheUpdateItem::AsyncOnChannelRedirect(nsIChannel *aOldChannel,
                                                 nsIChannel *aNewChannel,
                                                 PRUint32 aFlags,
                                                 nsIAsyncVerifyRedirectCallback *cb)
{
    if (!(aFlags & nsIChannelEventSink::REDIRECT_INTERNAL)) {
        // Don't allow redirect in case of non-internal redirect and cancel
        // the channel to clean the cache entry.
        aOldChannel->Cancel(NS_ERROR_ABORT);
        return NS_ERROR_ABORT;
    }

    nsCOMPtr<nsIURI> newURI;
    nsresult rv = aNewChannel->GetURI(getter_AddRefs(newURI));
    if (NS_FAILED(rv))
        return rv;

    nsCOMPtr<nsICachingChannel> oldCachingChannel =
        do_QueryInterface(aOldChannel);
    nsCOMPtr<nsICachingChannel> newCachingChannel =
        do_QueryInterface(aNewChannel);
    if (newCachingChannel) {
        rv = newCachingChannel->SetCacheForOfflineUse(PR_TRUE);
        NS_ENSURE_SUCCESS(rv, rv);
        if (!mClientID.IsEmpty()) {
            rv = newCachingChannel->SetOfflineCacheClientID(mClientID);
            NS_ENSURE_SUCCESS(rv, rv);
        }
    }

    nsCAutoString oldScheme;
    mURI->GetScheme(oldScheme);

    PRBool match;
    if (NS_FAILED(newURI->SchemeIs(oldScheme.get(), &match)) || !match) {
        LOG(("rejected: redirected to a different scheme\n"));
        return NS_ERROR_ABORT;
    }

    // HTTP request headers are not automatically forwarded to the new channel.
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aNewChannel);
    NS_ENSURE_STATE(httpChannel);

    httpChannel->SetRequestHeader(NS_LITERAL_CSTRING("X-Moz"),
                                  NS_LITERAL_CSTRING("offline-resource"),
                                  PR_FALSE);

    mChannel = aNewChannel;

    cb->OnRedirectVerifyCallback(NS_OK);
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdateItem::nsIDOMLoadStatus
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetSource(nsIDOMNode **aSource)
{
    *aSource = nsnull;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetUri(nsAString &aURI)
{
    nsCAutoString spec;
    nsresult rv = mURI->GetSpec(spec);
    NS_ENSURE_SUCCESS(rv, rv);

    CopyUTF8toUTF16(spec, aURI);
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetTotalSize(PRInt32 *aTotalSize)
{
    if (mChannel) {
        return mChannel->GetContentLength(aTotalSize);
    }

    *aTotalSize = -1;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetLoadedSize(PRInt32 *aLoadedSize)
{
    *aLoadedSize = mBytesRead;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetReadyState(PRUint16 *aReadyState)
{
    *aReadyState = mState;
    return NS_OK;
}

nsresult
nsOfflineCacheUpdateItem::GetRequestSucceeded(PRBool * succeeded)
{
    *succeeded = PR_FALSE;

    if (!mChannel)
        return NS_OK;

    nsresult rv;
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(mChannel, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    PRBool reqSucceeded;
    rv = httpChannel->GetRequestSucceeded(&reqSucceeded);
    if (NS_ERROR_NOT_AVAILABLE == rv)
        return NS_OK;
    NS_ENSURE_SUCCESS(rv, rv);

    if (!reqSucceeded) {
        LOG(("Request failed"));
        return NS_OK;
    }

    nsresult channelStatus;
    rv = httpChannel->GetStatus(&channelStatus);
    NS_ENSURE_SUCCESS(rv, rv);

    if (NS_FAILED(channelStatus)) {
        LOG(("Channel status=0x%08x", channelStatus));
        return NS_OK;
    }

    *succeeded = PR_TRUE;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdateItem::GetStatus(PRUint16 *aStatus)
{
    if (!mChannel) {
        *aStatus = 0;
        return NS_OK;
    }

    nsresult rv;
    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(mChannel, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    PRUint32 httpStatus;
    rv = httpChannel->GetResponseStatus(&httpStatus);
    if (rv == NS_ERROR_NOT_AVAILABLE) {
        *aStatus = 0;
        return NS_OK;
    }

    NS_ENSURE_SUCCESS(rv, rv);
    *aStatus = PRUint16(httpStatus);
    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsOfflineManifestItem
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// nsOfflineManifestItem <public>
//-----------------------------------------------------------------------------

nsOfflineManifestItem::nsOfflineManifestItem(nsOfflineCacheUpdate *aUpdate,
                                             nsIURI *aURI,
                                             nsIURI *aReferrerURI,
                                             nsIApplicationCache *aPreviousApplicationCache,
                                             const nsACString &aClientID)
    : nsOfflineCacheUpdateItem(aUpdate, aURI, aReferrerURI,
                               aPreviousApplicationCache, aClientID,
                               nsIApplicationCache::ITEM_MANIFEST)
    , mParserState(PARSE_INIT)
    , mNeedsUpdate(PR_TRUE)
    , mManifestHashInitialized(PR_FALSE)
{
    ReadStrictFileOriginPolicyPref();
}

nsOfflineManifestItem::~nsOfflineManifestItem()
{
}

//-----------------------------------------------------------------------------
// nsOfflineManifestItem <private>
//-----------------------------------------------------------------------------

/* static */
NS_METHOD
nsOfflineManifestItem::ReadManifest(nsIInputStream *aInputStream,
                                    void *aClosure,
                                    const char *aFromSegment,
                                    PRUint32 aOffset,
                                    PRUint32 aCount,
                                    PRUint32 *aBytesConsumed)
{
    nsOfflineManifestItem *manifest =
        static_cast<nsOfflineManifestItem*>(aClosure);

    nsresult rv;

    *aBytesConsumed = aCount;

    if (manifest->mParserState == PARSE_ERROR) {
        // parse already failed, ignore this
        return NS_OK;
    }

    if (!manifest->mManifestHashInitialized) {
        // Avoid re-creation of crypto hash when it fails from some reason the first time
        manifest->mManifestHashInitialized = PR_TRUE;

        manifest->mManifestHash = do_CreateInstance("@mozilla.org/security/hash;1", &rv);
        if (NS_SUCCEEDED(rv)) {
            rv = manifest->mManifestHash->Init(nsICryptoHash::MD5);
            if (NS_FAILED(rv)) {
                manifest->mManifestHash = nsnull;
                LOG(("Could not initialize manifest hash for byte-to-byte check, rv=%08x", rv));
            }
        }
    }

    if (manifest->mManifestHash) {
        rv = manifest->mManifestHash->Update(reinterpret_cast<const PRUint8 *>(aFromSegment), aCount);
        if (NS_FAILED(rv)) {
            manifest->mManifestHash = nsnull;
            LOG(("Could not update manifest hash, rv=%08x", rv));
        }
    }

    manifest->mReadBuf.Append(aFromSegment, aCount);

    nsCString::const_iterator begin, iter, end;
    manifest->mReadBuf.BeginReading(begin);
    manifest->mReadBuf.EndReading(end);

    for (iter = begin; iter != end; iter++) {
        if (*iter == '\r' || *iter == '\n') {
            nsresult rv = manifest->HandleManifestLine(begin, iter);
            
            if (NS_FAILED(rv)) {
                LOG(("HandleManifestLine failed with 0x%08x", rv));
                return NS_ERROR_ABORT;
            }

            begin = iter;
            begin++;
        }
    }

    // any leftovers are saved for next time
    manifest->mReadBuf = Substring(begin, end);

    return NS_OK;
}

nsresult
nsOfflineManifestItem::AddNamespace(PRUint32 namespaceType,
                                    const nsCString &namespaceSpec,
                                    const nsCString &data)

{
    nsresult rv;
    if (!mNamespaces) {
        mNamespaces = do_CreateInstance(NS_ARRAY_CONTRACTID, &rv);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    nsCOMPtr<nsIApplicationCacheNamespace> ns =
        do_CreateInstance(NS_APPLICATIONCACHENAMESPACE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = ns->Init(namespaceType, namespaceSpec, data);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = mNamespaces->AppendElement(ns, PR_FALSE);
    NS_ENSURE_SUCCESS(rv, rv);

    return NS_OK;
}

nsresult
nsOfflineManifestItem::HandleManifestLine(const nsCString::const_iterator &aBegin,
                                          const nsCString::const_iterator &aEnd)
{
    nsCString::const_iterator begin = aBegin;
    nsCString::const_iterator end = aEnd;

    // all lines ignore trailing spaces and tabs
    nsCString::const_iterator last = end;
    --last;
    while (end != begin && (*last == ' ' || *last == '\t')) {
        --end;
        --last;
    }

    if (mParserState == PARSE_INIT) {
        // Allow a UTF-8 BOM
        if (begin != end && static_cast<unsigned char>(*begin) == 0xef) {
            if (++begin == end || static_cast<unsigned char>(*begin) != 0xbb ||
                ++begin == end || static_cast<unsigned char>(*begin) != 0xbf) {
                mParserState = PARSE_ERROR;
                return NS_OK;
            }
            ++begin;
        }

        const nsCSubstring &magic = Substring(begin, end);

        if (!magic.EqualsLiteral("CACHE MANIFEST")) {
            mParserState = PARSE_ERROR;
            return NS_OK;
        }

        mParserState = PARSE_CACHE_ENTRIES;
        return NS_OK;
    }

    // lines other than the first ignore leading spaces and tabs
    while (begin != end && (*begin == ' ' || *begin == '\t'))
        begin++;

    // ignore blank lines and comments
    if (begin == end || *begin == '#')
        return NS_OK;

    const nsCSubstring &line = Substring(begin, end);

    if (line.EqualsLiteral("CACHE:")) {
        mParserState = PARSE_CACHE_ENTRIES;
        return NS_OK;
    }

    if (line.EqualsLiteral("FALLBACK:")) {
        mParserState = PARSE_FALLBACK_ENTRIES;
        return NS_OK;
    }

    if (line.EqualsLiteral("NETWORK:")) {
        mParserState = PARSE_BYPASS_ENTRIES;
        return NS_OK;
    }

    nsresult rv;

    switch(mParserState) {
    case PARSE_INIT:
    case PARSE_ERROR: {
        // this should have been dealt with earlier
        return NS_ERROR_FAILURE;
    }

    case PARSE_CACHE_ENTRIES: {
        nsCOMPtr<nsIURI> uri;
        rv = NS_NewURI(getter_AddRefs(uri), line, nsnull, mURI);
        if (NS_FAILED(rv))
            break;
        if (NS_FAILED(DropReferenceFromURL(uri)))
            break;

        nsCAutoString scheme;
        uri->GetScheme(scheme);

        // Manifest URIs must have the same scheme as the manifest.
        PRBool match;
        if (NS_FAILED(mURI->SchemeIs(scheme.get(), &match)) || !match)
            break;

        mExplicitURIs.AppendObject(uri);
        break;
    }

    case PARSE_FALLBACK_ENTRIES: {
        PRInt32 separator = line.FindChar(' ');
        if (separator == kNotFound) {
            separator = line.FindChar('\t');
            if (separator == kNotFound)
                break;
        }

        nsCString namespaceSpec(Substring(line, 0, separator));
        nsCString fallbackSpec(Substring(line, separator + 1));
        namespaceSpec.CompressWhitespace();
        fallbackSpec.CompressWhitespace();

        nsCOMPtr<nsIURI> namespaceURI;
        rv = NS_NewURI(getter_AddRefs(namespaceURI), namespaceSpec, nsnull, mURI);
        if (NS_FAILED(rv))
            break;
        if (NS_FAILED(DropReferenceFromURL(namespaceURI)))
            break;
        rv = namespaceURI->GetAsciiSpec(namespaceSpec);
        if (NS_FAILED(rv))
            break;


        nsCOMPtr<nsIURI> fallbackURI;
        rv = NS_NewURI(getter_AddRefs(fallbackURI), fallbackSpec, nsnull, mURI);
        if (NS_FAILED(rv))
            break;
        if (NS_FAILED(DropReferenceFromURL(fallbackURI)))
            break;
        rv = fallbackURI->GetAsciiSpec(fallbackSpec);
        if (NS_FAILED(rv))
            break;

        // Manifest and namespace must be same origin
        if (!NS_SecurityCompareURIs(mURI, namespaceURI,
                                    mStrictFileOriginPolicy))
            break;

        // Fallback and namespace must be same origin
        if (!NS_SecurityCompareURIs(namespaceURI, fallbackURI,
                                    mStrictFileOriginPolicy))
            break;

        mFallbackURIs.AppendObject(fallbackURI);

        AddNamespace(nsIApplicationCacheNamespace::NAMESPACE_FALLBACK,
                     namespaceSpec, fallbackSpec);
        break;
    }

    case PARSE_BYPASS_ENTRIES: {
        if (line[0] == '*' && (line.Length() == 1 || line[1] == ' ' || line[1] == '\t'))
        {
          // '*' indicates to make the online whitelist wildcard flag open,
          // i.e. do allow load of resources not present in the offline cache
          // or not conforming any namespace.
          // We achive that simply by adding an 'empty' - i.e. universal
          // namespace of BYPASS type into the cache.
          AddNamespace(nsIApplicationCacheNamespace::NAMESPACE_BYPASS,
                       EmptyCString(), EmptyCString());
          break;
        }

        nsCOMPtr<nsIURI> bypassURI;
        rv = NS_NewURI(getter_AddRefs(bypassURI), line, nsnull, mURI);
        if (NS_FAILED(rv))
            break;

        nsCAutoString scheme;
        bypassURI->GetScheme(scheme);
        PRBool equals;
        if (NS_FAILED(mURI->SchemeIs(scheme.get(), &equals)) || !equals)
            break;
        if (NS_FAILED(DropReferenceFromURL(bypassURI)))
            break;
        nsCString spec;
        if (NS_FAILED(bypassURI->GetAsciiSpec(spec)))
            break;

        AddNamespace(nsIApplicationCacheNamespace::NAMESPACE_BYPASS,
                     spec, EmptyCString());
        break;
    }
    }

    return NS_OK;
}

nsresult 
nsOfflineManifestItem::GetOldManifestContentHash(nsIRequest *aRequest)
{
    nsresult rv;

    nsCOMPtr<nsICachingChannel> cachingChannel = do_QueryInterface(aRequest, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // load the main cache token that is actually the old offline cache token and 
    // read previous manifest content hash value
    nsCOMPtr<nsISupports> cacheToken;
    cachingChannel->GetCacheToken(getter_AddRefs(cacheToken));
    if (cacheToken) {
        nsCOMPtr<nsICacheEntryDescriptor> cacheDescriptor(do_QueryInterface(cacheToken, &rv));
        NS_ENSURE_SUCCESS(rv, rv);
    
        rv = cacheDescriptor->GetMetaDataElement("offline-manifest-hash", getter_Copies(mOldManifestHashValue));
        if (NS_FAILED(rv))
            mOldManifestHashValue.Truncate();
    }

    return NS_OK;
}

nsresult 
nsOfflineManifestItem::CheckNewManifestContentHash(nsIRequest *aRequest)
{
    nsresult rv;

    if (!mManifestHash) {
        // Nothing to compare against...
        return NS_OK;
    }

    nsCString newManifestHashValue;
    rv = mManifestHash->Finish(PR_TRUE, mManifestHashValue);
    mManifestHash = nsnull;

    if (NS_FAILED(rv)) {
        LOG(("Could not finish manifest hash, rv=%08x", rv));
        // This is not critical error
        return NS_OK;
    }

    if (!ParseSucceeded()) {
        // Parsing failed, the hash is not valid
        return NS_OK;
    }

    if (mOldManifestHashValue == mManifestHashValue) {
        LOG(("Update not needed, downloaded manifest content is byte-for-byte identical"));
        mNeedsUpdate = PR_FALSE;
    }

    // Store the manifest content hash value to the new
    // offline cache token
    nsCOMPtr<nsICachingChannel> cachingChannel = do_QueryInterface(aRequest, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsISupports> cacheToken;
    cachingChannel->GetOfflineCacheToken(getter_AddRefs(cacheToken));
    if (cacheToken) {
        nsCOMPtr<nsICacheEntryDescriptor> cacheDescriptor(do_QueryInterface(cacheToken, &rv));
        NS_ENSURE_SUCCESS(rv, rv);
    
        rv = cacheDescriptor->SetMetaDataElement("offline-manifest-hash", mManifestHashValue.get());
        NS_ENSURE_SUCCESS(rv, rv);
    }

    return NS_OK;
}

void
nsOfflineManifestItem::ReadStrictFileOriginPolicyPref()
{
    nsCOMPtr<nsIPrefBranch> prefs = do_GetService(NS_PREFSERVICE_CONTRACTID);
    mStrictFileOriginPolicy =
        (!prefs ||
         NS_FAILED(prefs->GetBoolPref("security.fileuri.strict_origin_policy",
                                      &mStrictFileOriginPolicy)));
}

NS_IMETHODIMP
nsOfflineManifestItem::OnStartRequest(nsIRequest *aRequest,
                                      nsISupports *aContext)
{
    nsresult rv;

    nsCOMPtr<nsIHttpChannel> channel = do_QueryInterface(aRequest, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    PRBool succeeded;
    rv = channel->GetRequestSucceeded(&succeeded);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!succeeded) {
        LOG(("HTTP request failed"));
        mParserState = PARSE_ERROR;
        return NS_ERROR_ABORT;
    }

    nsCAutoString contentType;
    rv = channel->GetContentType(contentType);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!contentType.EqualsLiteral("text/cache-manifest")) {
        LOG(("Rejected cache manifest with Content-Type %s (expecting text/cache-manifest)",
             contentType.get()));
        mParserState = PARSE_ERROR;
        return NS_ERROR_ABORT;
    }

    rv = GetOldManifestContentHash(aRequest);
    NS_ENSURE_SUCCESS(rv, rv);

    return nsOfflineCacheUpdateItem::OnStartRequest(aRequest, aContext);
}

NS_IMETHODIMP
nsOfflineManifestItem::OnDataAvailable(nsIRequest *aRequest,
                                       nsISupports *aContext,
                                       nsIInputStream *aStream,
                                       PRUint32 aOffset,
                                       PRUint32 aCount)
{
    PRUint32 bytesRead = 0;
    aStream->ReadSegments(ReadManifest, this, aCount, &bytesRead);
    mBytesRead += bytesRead;

    if (mParserState == PARSE_ERROR) {
        LOG(("OnDataAvailable is canceling the request due a parse error\n"));
        return NS_ERROR_ABORT;
    }

    LOG(("loaded %u bytes into offline cache [offset=%u]\n",
         bytesRead, aOffset));

    // All the parent method does is read and discard, don't bother
    // chaining up.

    return NS_OK;
}

NS_IMETHODIMP
nsOfflineManifestItem::OnStopRequest(nsIRequest *aRequest,
                                     nsISupports *aContext,
                                     nsresult aStatus)
{
    // handle any leftover manifest data
    nsCString::const_iterator begin, end;
    mReadBuf.BeginReading(begin);
    mReadBuf.EndReading(end);
    nsresult rv = HandleManifestLine(begin, end);
    NS_ENSURE_SUCCESS(rv, rv);

    if (mBytesRead == 0) {
        // we didn't need to read (because LOAD_ONLY_IF_MODIFIED was
        // specified.)
        mNeedsUpdate = PR_FALSE;
    } else {
        rv = CheckNewManifestContentHash(aRequest);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    return nsOfflineCacheUpdateItem::OnStopRequest(aRequest, aContext, aStatus);
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdate::nsISupports
//-----------------------------------------------------------------------------

NS_IMPL_ISUPPORTS2(nsOfflineCacheUpdate,
                   nsIOfflineCacheUpdateObserver,
                   nsIOfflineCacheUpdate)

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdate <public>
//-----------------------------------------------------------------------------

nsOfflineCacheUpdate::nsOfflineCacheUpdate()
    : mState(STATE_UNINITIALIZED)
    , mOwner(nsnull)
    , mAddedItems(PR_FALSE)
    , mPartialUpdate(PR_FALSE)
    , mSucceeded(PR_TRUE)
    , mObsolete(PR_FALSE)
    , mCurrentItem(-1)
    , mRescheduleCount(0)
{
}

nsOfflineCacheUpdate::~nsOfflineCacheUpdate()
{
    LOG(("nsOfflineCacheUpdate::~nsOfflineCacheUpdate [%p]", this));
}

/* static */
nsresult
nsOfflineCacheUpdate::GetCacheKey(nsIURI *aURI, nsACString &aKey)
{
    aKey.Truncate();

    nsCOMPtr<nsIURI> newURI;
    nsresult rv = aURI->Clone(getter_AddRefs(newURI));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIURL> newURL;
    newURL = do_QueryInterface(newURI);
    if (newURL) {
        newURL->SetRef(EmptyCString());
    }

    rv = newURI->GetAsciiSpec(aKey);
    NS_ENSURE_SUCCESS(rv, rv);

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::Init(nsIURI *aManifestURI,
                           nsIURI *aDocumentURI,
                           nsIDOMDocument *aDocument)
{
    nsresult rv;

    // Make sure the service has been initialized
    nsOfflineCacheUpdateService* service =
        nsOfflineCacheUpdateService::EnsureService();
    if (!service)
        return NS_ERROR_FAILURE;

    LOG(("nsOfflineCacheUpdate::Init [%p]", this));

    mPartialUpdate = PR_FALSE;

    // Only http and https applications are supported.
    PRBool match;
    rv = aManifestURI->SchemeIs("http", &match);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!match) {
        rv = aManifestURI->SchemeIs("https", &match);
        NS_ENSURE_SUCCESS(rv, rv);
        if (!match)
            return NS_ERROR_ABORT;
    }

    mManifestURI = aManifestURI;

    rv = mManifestURI->GetAsciiHost(mUpdateDomain);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCAutoString manifestSpec;

    rv = GetCacheKey(mManifestURI, manifestSpec);
    NS_ENSURE_SUCCESS(rv, rv);

    mDocumentURI = aDocumentURI;

    nsCOMPtr<nsIApplicationCacheService> cacheService =
        do_GetService(NS_APPLICATIONCACHESERVICE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = cacheService->GetActiveCache(manifestSpec,
                                      getter_AddRefs(mPreviousApplicationCache));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = cacheService->CreateApplicationCache(manifestSpec,
                                              getter_AddRefs(mApplicationCache));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = mApplicationCache->GetClientID(mClientID);
    NS_ENSURE_SUCCESS(rv, rv);

    mState = STATE_INITIALIZED;
    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::InitPartial(nsIURI *aManifestURI,
                                  const nsACString& clientID,
                                  nsIURI *aDocumentURI)
{
    nsresult rv;

    // Make sure the service has been initialized
    nsOfflineCacheUpdateService* service =
        nsOfflineCacheUpdateService::EnsureService();
    if (!service)
        return NS_ERROR_FAILURE;

    LOG(("nsOfflineCacheUpdate::InitPartial [%p]", this));

    mPartialUpdate = PR_TRUE;
    mClientID = clientID;
    mDocumentURI = aDocumentURI;

    mManifestURI = aManifestURI;
    rv = mManifestURI->GetAsciiHost(mUpdateDomain);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIApplicationCacheService> cacheService =
        do_GetService(NS_APPLICATIONCACHESERVICE_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = cacheService->GetApplicationCache(mClientID,
                                           getter_AddRefs(mApplicationCache));
    NS_ENSURE_SUCCESS(rv, rv);

    if (!mApplicationCache) {
        nsCAutoString manifestSpec;
        rv = GetCacheKey(mManifestURI, manifestSpec);
        NS_ENSURE_SUCCESS(rv, rv);

        rv = cacheService->CreateApplicationCache
            (manifestSpec, getter_AddRefs(mApplicationCache));
        NS_ENSURE_SUCCESS(rv, rv);
    }

    nsCAutoString groupID;
    rv = mApplicationCache->GetGroupID(groupID);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_NewURI(getter_AddRefs(mManifestURI), groupID);
    NS_ENSURE_SUCCESS(rv, rv);

    mState = STATE_INITIALIZED;
    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::HandleManifest(PRBool *aDoUpdate)
{
    // Be pessimistic
    *aDoUpdate = PR_FALSE;

    PRBool succeeded;
    nsresult rv = mManifestItem->GetRequestSucceeded(&succeeded);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!succeeded || !mManifestItem->ParseSucceeded()) {
        return NS_ERROR_FAILURE;
    }

    if (!mManifestItem->NeedsUpdate()) {
        return NS_OK;
    }

    // Add items requested by the manifest.
    const nsCOMArray<nsIURI> &manifestURIs = mManifestItem->GetExplicitURIs();
    for (PRInt32 i = 0; i < manifestURIs.Count(); i++) {
        rv = AddURI(manifestURIs[i], nsIApplicationCache::ITEM_EXPLICIT);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    const nsCOMArray<nsIURI> &fallbackURIs = mManifestItem->GetFallbackURIs();
    for (PRInt32 i = 0; i < fallbackURIs.Count(); i++) {
        rv = AddURI(fallbackURIs[i], nsIApplicationCache::ITEM_FALLBACK);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    // The document that requested the manifest is implicitly included
    // as part of that manifest update.
    rv = AddURI(mDocumentURI, nsIApplicationCache::ITEM_IMPLICIT);
    NS_ENSURE_SUCCESS(rv, rv);

    // Add items previously cached implicitly
    rv = AddExistingItems(nsIApplicationCache::ITEM_IMPLICIT);
    NS_ENSURE_SUCCESS(rv, rv);

    // Add items requested by the script API
    rv = AddExistingItems(nsIApplicationCache::ITEM_DYNAMIC);
    NS_ENSURE_SUCCESS(rv, rv);

    // Add opportunistically cached items conforming current opportunistic
    // namespace list
    rv = AddExistingItems(nsIApplicationCache::ITEM_OPPORTUNISTIC,
                          &mManifestItem->GetOpportunisticNamespaces());
    NS_ENSURE_SUCCESS(rv, rv);

    *aDoUpdate = PR_TRUE;

    return NS_OK;
}

void
nsOfflineCacheUpdate::LoadCompleted()
{
    nsresult rv;

    // Keep the object alive through a Finish() call.
    nsCOMPtr<nsIOfflineCacheUpdate> kungFuDeathGrip(this);

    LOG(("nsOfflineCacheUpdate::LoadCompleted [%p]", this));

    if (mState == STATE_CANCELLED) {
        Finish();
        return;
    }

    if (mState == STATE_CHECKING) {
        // Manifest load finished.

        NS_ASSERTION(mManifestItem,
                     "Must have a manifest item in STATE_CHECKING.");

        // A 404 or 410 is interpreted as an intentional removal of
        // the manifest file, rather than a transient server error.
        // Obsolete this cache group if one of these is returned.
        PRUint16 status;
        rv = mManifestItem->GetStatus(&status);
        if (status == 404 || status == 410) {
            mSucceeded = PR_FALSE;
            mObsolete = PR_TRUE;
            if (mPreviousApplicationCache) {
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_OBSOLETE);
            } else {
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
            }
            Finish();
            return;
        }

        PRBool doUpdate;
        if (NS_FAILED(HandleManifest(&doUpdate))) {
            mSucceeded = PR_FALSE;
            NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
            Finish();
            return;
        }

        if (!doUpdate) {
            mSucceeded = PR_FALSE;

            AssociateDocuments(mPreviousApplicationCache);

            ScheduleImplicit();

            // If we didn't need an implicit update, we can
            // send noupdate and end the update now.
            if (!mImplicitUpdate) {
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_NOUPDATE);
                Finish();
            }
            return;
        }

        rv = mApplicationCache->MarkEntry(mManifestItem->mCacheKey,
                                          mManifestItem->mItemType);
        if (NS_FAILED(rv)) {
            mSucceeded = PR_FALSE;
            NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
            Finish();
            return;
        }

        mState = STATE_DOWNLOADING;
        NotifyState(nsIOfflineCacheUpdateObserver::STATE_DOWNLOADING);

        // Start fetching resources.
        ProcessNextURI();

        return;
    }

    // Normal load finished.

    nsRefPtr<nsOfflineCacheUpdateItem> item = mItems[mCurrentItem];
    mCurrentItem++;

    PRBool succeeded;
    rv = item->GetRequestSucceeded(&succeeded);

    // Check for failures.  3XX, 4XX and 5XX errors on items explicitly
    // listed in the manifest will cause the update to fail.
    if (NS_FAILED(rv) || !succeeded) {
        if (item->mItemType &
            (nsIApplicationCache::ITEM_EXPLICIT |
             nsIApplicationCache::ITEM_FALLBACK)) {
            mSucceeded = PR_FALSE;
        }
    } else {
        rv = mApplicationCache->MarkEntry(item->mCacheKey, item->mItemType);
        if (NS_FAILED(rv)) {
            mSucceeded = PR_FALSE;
        }
    }

    if (!mSucceeded) {
        NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
        Finish();
        return;
    }

    rv = NotifyState(nsIOfflineCacheUpdateObserver::STATE_ITEMCOMPLETED);
    if (NS_FAILED(rv)) return;

    ProcessNextURI();
}

void
nsOfflineCacheUpdate::ManifestCheckCompleted(nsresult aStatus,
                                             const nsCString &aManifestHash)
{
    // Keep the object alive through a Finish() call.
    nsCOMPtr<nsIOfflineCacheUpdate> kungFuDeathGrip(this);

    if (NS_SUCCEEDED(aStatus)) {
        nsCAutoString firstManifestHash;
        mManifestItem->GetManifestHash(firstManifestHash);
        if (aManifestHash != firstManifestHash) {
            LOG(("Manifest has changed during cache items download [%p]", this));
            aStatus = NS_ERROR_FAILURE;
        }
    }

    if (NS_FAILED(aStatus)) {
        mSucceeded = PR_FALSE;
        NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
    }

    if (NS_FAILED(aStatus) && mRescheduleCount < kRescheduleLimit) {
        // Do the final stuff but prevent notification of STATE_FINISHED.  
        // That would disconnect listeners that are responsible for document
        // association after a successful update. Forwarding notifications
        // from a new update through this dead update to them is absolutely
        // correct.
        FinishNoNotify();

        nsRefPtr<nsOfflineCacheUpdate> newUpdate =
            new nsOfflineCacheUpdate();
        // Leave aDocument argument null. Only glues and children keep 
        // document instances.
        newUpdate->Init(mManifestURI, mDocumentURI, nsnull);

        // In a rare case the manifest will not be modified on the next refetch
        // transfer all master document URIs to the new update to ensure that
        // all documents refering it will be properly cached.
        for (PRInt32 i = 0; i < mDocumentURIs.Count(); i++) {
            newUpdate->StickDocument(mDocumentURIs[i]);
        }

        newUpdate->mRescheduleCount = mRescheduleCount + 1;
        newUpdate->AddObserver(this, PR_FALSE);
        newUpdate->Schedule();
    }
    else {
        Finish();
    }
}

nsresult
nsOfflineCacheUpdate::Begin()
{
    LOG(("nsOfflineCacheUpdate::Begin [%p]", this));

    // Keep the object alive through a ProcessNextURI()/Finish() call.
    nsCOMPtr<nsIOfflineCacheUpdate> kungFuDeathGrip(this);

    mCurrentItem = 0;

    if (mPartialUpdate) {
        mState = STATE_DOWNLOADING;
        NotifyState(nsIOfflineCacheUpdateObserver::STATE_DOWNLOADING);
        ProcessNextURI();
        return NS_OK;
    }

    // Start checking the manifest.
    nsCOMPtr<nsIURI> uri;

    mManifestItem = new nsOfflineManifestItem(this, mManifestURI,
                                              mDocumentURI,
                                              mPreviousApplicationCache,
                                              mClientID);
    if (!mManifestItem) {
        return NS_ERROR_OUT_OF_MEMORY;
    }

    mState = STATE_CHECKING;
    NotifyState(nsIOfflineCacheUpdateObserver::STATE_CHECKING);

    nsresult rv = mManifestItem->OpenChannel();
    if (NS_FAILED(rv)) {
        LoadCompleted();
    }

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::Cancel()
{
    LOG(("nsOfflineCacheUpdate::Cancel [%p]", this));

    mState = STATE_CANCELLED;
    mSucceeded = PR_FALSE;

    if (mCurrentItem >= 0 &&
        mCurrentItem < static_cast<PRInt32>(mItems.Length())) {
        // Load might be running
        mItems[mCurrentItem]->Cancel();
    }

    return NS_OK;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdate <private>
//-----------------------------------------------------------------------------

nsresult
nsOfflineCacheUpdate::AddExistingItems(PRUint32 aType,
                                       nsTArray<nsCString>* namespaceFilter)
{
    if (!mPreviousApplicationCache) {
        return NS_OK;
    }

    if (namespaceFilter && namespaceFilter->Length() == 0) {
        // Don't bother to walk entries when there are no namespaces
        // defined.
        return NS_OK;
    }

    PRUint32 count = 0;
    char **keys = nsnull;
    nsresult rv = mPreviousApplicationCache->GatherEntries(aType,
                                                           &count, &keys);
    NS_ENSURE_SUCCESS(rv, rv);

    AutoFreeArray autoFree(count, keys);

    for (PRUint32 i = 0; i < count; i++) {
        if (namespaceFilter) {
            PRBool found = PR_FALSE;
            for (PRUint32 j = 0; j < namespaceFilter->Length() && !found; j++) {
                found = StringBeginsWith(nsDependentCString(keys[i]),
                                         namespaceFilter->ElementAt(j));
            }

            if (!found)
                continue;
        }

        nsCOMPtr<nsIURI> uri;
        if (NS_SUCCEEDED(NS_NewURI(getter_AddRefs(uri), keys[i]))) {
            rv = AddURI(uri, aType);
            NS_ENSURE_SUCCESS(rv, rv);
        }
    }

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::ProcessNextURI()
{
    // Keep the object alive through a Finish() call.
    nsCOMPtr<nsIOfflineCacheUpdate> kungFuDeathGrip(this);

    LOG(("nsOfflineCacheUpdate::ProcessNextURI [%p, current=%d, numItems=%d]",
         this, mCurrentItem, mItems.Length()));

    NS_ASSERTION(mState == STATE_DOWNLOADING,
                 "ProcessNextURI should only be called from the DOWNLOADING state");

    if (mCurrentItem >= static_cast<PRInt32>(mItems.Length())) {
        if (mPartialUpdate) {
            return Finish();
        } else {
            // Verify that the manifest wasn't changed during the
            // update, to prevent capturing a cache while the server
            // is being updated.  The check will call
            // ManifestCheckCompleted() when it's done.
            nsRefPtr<nsManifestCheck> manifestCheck =
                new nsManifestCheck(this, mManifestURI, mDocumentURI);
            if (NS_FAILED(manifestCheck->Begin())) {
                mSucceeded = PR_FALSE;
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
                return Finish();
            }

            return NS_OK;
        }
    }

#if defined(PR_LOGGING)
    if (LOG_ENABLED()) {
        nsCAutoString spec;
        mItems[mCurrentItem]->mURI->GetSpec(spec);
        LOG(("%p: Opening channel for %s", this, spec.get()));
    }
#endif

    NotifyState(nsIOfflineCacheUpdateObserver::STATE_ITEMSTARTED);

    nsresult rv = mItems[mCurrentItem]->OpenChannel();
    if (NS_FAILED(rv)) {
        LoadCompleted();
        return rv;
    }

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::GatherObservers(nsCOMArray<nsIOfflineCacheUpdateObserver> &aObservers)
{
    for (PRInt32 i = 0; i < mWeakObservers.Count(); i++) {
        nsCOMPtr<nsIOfflineCacheUpdateObserver> observer =
            do_QueryReferent(mWeakObservers[i]);
        if (observer)
            aObservers.AppendObject(observer);
        else
            mWeakObservers.RemoveObjectAt(i--);
    }

    for (PRInt32 i = 0; i < mObservers.Count(); i++) {
        aObservers.AppendObject(mObservers[i]);
    }

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::NotifyState(PRUint32 state)
{
    LOG(("nsOfflineCacheUpdate::NotifyState [%p, %d]", this, state));

    nsCOMArray<nsIOfflineCacheUpdateObserver> observers;
    nsresult rv = GatherObservers(observers);
    NS_ENSURE_SUCCESS(rv, rv);

    for (PRInt32 i = 0; i < observers.Count(); i++) {
        observers[i]->UpdateStateChanged(this, state);
    }

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::AssociateDocuments(nsIApplicationCache* cache)
{
    nsCOMArray<nsIOfflineCacheUpdateObserver> observers;
    nsresult rv = GatherObservers(observers);
    NS_ENSURE_SUCCESS(rv, rv);

    for (PRInt32 i = 0; i < observers.Count(); i++) {
        observers[i]->ApplicationCacheAvailable(cache);
    }

    return NS_OK;
}

void
nsOfflineCacheUpdate::StickDocument(nsIURI *aDocumentURI)
{
    if (!aDocumentURI)
      return;

    mDocumentURIs.AppendObject(aDocumentURI);
}

void
nsOfflineCacheUpdate::SetOwner(nsOfflineCacheUpdateOwner *aOwner)
{
    NS_ASSERTION(!mOwner, "Tried to set cache update owner twice.");
    mOwner = aOwner;
}

nsresult
nsOfflineCacheUpdate::UpdateFinished(nsOfflineCacheUpdate *aUpdate)
{
    // Keep the object alive through a Finish() call.
    nsCOMPtr<nsIOfflineCacheUpdate> kungFuDeathGrip(this);

    mImplicitUpdate = nsnull;

    NotifyState(nsIOfflineCacheUpdateObserver::STATE_NOUPDATE);
    Finish();

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::ScheduleImplicit()
{
    if (mDocumentURIs.Count() == 0)
        return NS_OK;

    nsresult rv;

    nsRefPtr<nsOfflineCacheUpdate> update = new nsOfflineCacheUpdate();
    NS_ENSURE_TRUE(update, NS_ERROR_OUT_OF_MEMORY);

    nsCAutoString clientID;
    if (mPreviousApplicationCache) {
        rv = mPreviousApplicationCache->GetClientID(clientID);
        NS_ENSURE_SUCCESS(rv, rv);
    }
    else {
        clientID = mClientID;
    }

    rv = update->InitPartial(mManifestURI, clientID, mDocumentURI);
    NS_ENSURE_SUCCESS(rv, rv);

    for (PRInt32 i = 0; i < mDocumentURIs.Count(); i++) {
        rv = update->AddURI(mDocumentURIs[i], 
              nsIApplicationCache::ITEM_IMPLICIT);
        NS_ENSURE_SUCCESS(rv, rv);
    }

    update->SetOwner(this);
    rv = update->Begin();
    NS_ENSURE_SUCCESS(rv, rv);

    mImplicitUpdate = update;

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::FinishNoNotify()
{
    LOG(("nsOfflineCacheUpdate::Finish [%p]", this));

    mState = STATE_FINISHED;

    if (!mPartialUpdate) {
        if (mSucceeded) {
            nsIArray *namespaces = mManifestItem->GetNamespaces();
            nsresult rv = mApplicationCache->AddNamespaces(namespaces);
            if (NS_FAILED(rv)) {
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
                mSucceeded = PR_FALSE;
            }

            rv = mApplicationCache->Activate();
            if (NS_FAILED(rv)) {
                NotifyState(nsIOfflineCacheUpdateObserver::STATE_ERROR);
                mSucceeded = PR_FALSE;
            }

            AssociateDocuments(mApplicationCache);
        }

        if (mObsolete) {
            nsCOMPtr<nsIApplicationCacheService> appCacheService =
                do_GetService(NS_APPLICATIONCACHESERVICE_CONTRACTID);
            if (appCacheService) {
                nsCAutoString groupID;
                mApplicationCache->GetGroupID(groupID);
                appCacheService->DeactivateGroup(groupID);
             }
         }

        if (!mSucceeded) {
            // Update was not merged, mark all the loads as failures
            for (PRUint32 i = 0; i < mItems.Length(); i++) {
                mItems[i]->Cancel();
            }

            mApplicationCache->Discard();
        }
    }

    nsresult rv = NS_OK;

    if (mOwner) {
        rv = mOwner->UpdateFinished(this);
        mOwner = nsnull;
    }

    return rv;
}

nsresult
nsOfflineCacheUpdate::Finish()
{
    nsresult rv = FinishNoNotify();

    NotifyState(nsIOfflineCacheUpdateObserver::STATE_FINISHED);

    return rv;
}

//-----------------------------------------------------------------------------
// nsOfflineCacheUpdate::nsIOfflineCacheUpdate
//-----------------------------------------------------------------------------

NS_IMETHODIMP
nsOfflineCacheUpdate::GetUpdateDomain(nsACString &aUpdateDomain)
{
    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    aUpdateDomain = mUpdateDomain;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::GetStatus(PRUint16 *aStatus)
{
    switch (mState) {
    case STATE_CHECKING :
        *aStatus = nsIDOMOfflineResourceList::CHECKING;
        return NS_OK;
    case STATE_DOWNLOADING :
        *aStatus = nsIDOMOfflineResourceList::DOWNLOADING;
        return NS_OK;
    default :
        *aStatus = nsIDOMOfflineResourceList::IDLE;
        return NS_OK;
    }

    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::GetPartial(PRBool *aPartial)
{
    *aPartial = mPartialUpdate;
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::GetManifestURI(nsIURI **aManifestURI)
{
    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    NS_IF_ADDREF(*aManifestURI = mManifestURI);
    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::GetSucceeded(PRBool *aSucceeded)
{
    NS_ENSURE_TRUE(mState == STATE_FINISHED, NS_ERROR_NOT_AVAILABLE);

    *aSucceeded = mSucceeded;

    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::GetIsUpgrade(PRBool *aIsUpgrade)
{
    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    *aIsUpgrade = (mPreviousApplicationCache != nsnull);

    return NS_OK;
}

nsresult
nsOfflineCacheUpdate::AddURI(nsIURI *aURI, PRUint32 aType)
{
    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    if (mState >= STATE_DOWNLOADING)
        return NS_ERROR_NOT_AVAILABLE;

    // Resource URIs must have the same scheme as the manifest.
    nsCAutoString scheme;
    aURI->GetScheme(scheme);

    PRBool match;
    if (NS_FAILED(mManifestURI->SchemeIs(scheme.get(), &match)) || !match)
        return NS_ERROR_FAILURE;

    // Don't fetch the same URI twice.
    for (PRUint32 i = 0; i < mItems.Length(); i++) {
        PRBool equals;
        if (NS_SUCCEEDED(mItems[i]->mURI->Equals(aURI, &equals)) && equals) {
            // retain both types.
            mItems[i]->mItemType |= aType;
            return NS_OK;
        }
    }

    nsRefPtr<nsOfflineCacheUpdateItem> item =
        new nsOfflineCacheUpdateItem(this, aURI, mDocumentURI,
                                     mPreviousApplicationCache, mClientID,
                                     aType);
    if (!item) return NS_ERROR_OUT_OF_MEMORY;

    mItems.AppendElement(item);
    mAddedItems = PR_TRUE;

    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::AddDynamicURI(nsIURI *aURI)
{
    if (GeckoProcessType_Default != XRE_GetProcessType()) 
        return NS_ERROR_NOT_IMPLEMENTED;

    // If this is a partial update and the resource is already in the
    // cache, we should only mark the entry, not fetch it again.
    if (mPartialUpdate) {
        nsCAutoString key;
        GetCacheKey(aURI, key);

        PRUint32 types;
        nsresult rv = mApplicationCache->GetTypes(key, &types);
        if (NS_SUCCEEDED(rv)) {
            if (!(types & nsIApplicationCache::ITEM_DYNAMIC)) {
                mApplicationCache->MarkEntry
                    (key, nsIApplicationCache::ITEM_DYNAMIC);
            }
            return NS_OK;
        }
    }

    return AddURI(aURI, nsIApplicationCache::ITEM_DYNAMIC);
}

NS_IMETHODIMP
nsOfflineCacheUpdate::AddObserver(nsIOfflineCacheUpdateObserver *aObserver,
                                  PRBool aHoldWeak)
{
    LOG(("nsOfflineCacheUpdate::AddObserver [%p] to update [%p]", aObserver, this));

    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    if (aHoldWeak) {
        nsCOMPtr<nsIWeakReference> weakRef = do_GetWeakReference(aObserver);
        mWeakObservers.AppendObject(weakRef);
    } else {
        mObservers.AppendObject(aObserver);
    }

    return NS_OK;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::RemoveObserver(nsIOfflineCacheUpdateObserver *aObserver)
{
    LOG(("nsOfflineCacheUpdate::RemoveObserver [%p] from update [%p]", aObserver, this));

    NS_ENSURE_TRUE(mState >= STATE_INITIALIZED, NS_ERROR_NOT_INITIALIZED);

    for (PRInt32 i = 0; i < mWeakObservers.Count(); i++) {
        nsCOMPtr<nsIOfflineCacheUpdateObserver> observer =
            do_QueryReferent(mWeakObservers[i]);
        if (observer == aObserver) {
            mWeakObservers.RemoveObjectAt(i);
            return NS_OK;
        }
    }

    for (PRInt32 i = 0; i < mObservers.Count(); i++) {
        if (mObservers[i] == aObserver) {
            mObservers.RemoveObjectAt(i);
            return NS_OK;
        }
    }

    return NS_OK;
}


NS_IMETHODIMP
nsOfflineCacheUpdate::Schedule()
{
    LOG(("nsOfflineCacheUpdate::Schedule [%p]", this));

    nsOfflineCacheUpdateService* service =
        nsOfflineCacheUpdateService::EnsureService();

    if (!service) {
        return NS_ERROR_FAILURE;
    }

    return service->ScheduleUpdate(this);
}

NS_IMETHODIMP
nsOfflineCacheUpdate::UpdateStateChanged(nsIOfflineCacheUpdate *aUpdate,
                                         PRUint32 aState)
{
    if (aState == nsIOfflineCacheUpdateObserver::STATE_FINISHED) {
        // Take the mSucceeded flag from the underlying update, we will be
        // queried for it soon. mSucceeded of this update is false (manifest 
        // check failed) but the subsequent re-fetch update might succeed
        PRBool succeeded;
        aUpdate->GetSucceeded(&succeeded);
        mSucceeded = succeeded;
    }

    nsresult rv = NotifyState(aState);
    if (aState == nsIOfflineCacheUpdateObserver::STATE_FINISHED)
        aUpdate->RemoveObserver(this);

    return rv;
}

NS_IMETHODIMP
nsOfflineCacheUpdate::ApplicationCacheAvailable(nsIApplicationCache *applicationCache)
{
    return AssociateDocuments(applicationCache);
}
