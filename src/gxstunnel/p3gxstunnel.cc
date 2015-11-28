/*
 * libretroshare/src/chat: distantchat.cc
 *
 * Services for RetroShare.
 *
 * Copyright 2014 by Cyril Soler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License Version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA.
 *
 * Please report all bugs and problems to "csoler@users.sourceforge.net".
 *
 */


#include <unistd.h>

#include "openssl/rand.h"
#include "openssl/dh.h"
#include "openssl/err.h"

#include "util/rsaes.h"
#include "util/rsprint.h"
#include "util/rsmemory.h"

#include <retroshare/rsidentity.h>
#include <retroshare/rsiface.h>

#include <rsserver/p3face.h>
#include <services/p3idservice.h>
#include <gxs/gxssecurity.h>
#include <turtle/p3turtle.h>
#include <retroshare/rsids.h>

#include "p3gxstunnel.h"

#define DEBUG_GXS_TUNNEL

static const uint32_t GXS_TUNNEL_KEEP_ALIVE_TIMEOUT = 6 ; // send keep alive packet so as to avoid tunnel breaks.

static const uint32_t RS_GXS_TUNNEL_DH_STATUS_UNINITIALIZED = 0x0000 ;
static const uint32_t RS_GXS_TUNNEL_DH_STATUS_HALF_KEY_DONE = 0x0001 ;
static const uint32_t RS_GXS_TUNNEL_DH_STATUS_KEY_AVAILABLE = 0x0002 ;

static const uint32_t RS_GXS_TUNNEL_DELAY_BETWEEN_RESEND  = 10 ; // re-send every 10 secs.

static const uint32_t GXS_TUNNEL_ENCRYPTION_HMAC_SIZE    = SHA_DIGEST_LENGTH ;
static const uint32_t GXS_TUNNEL_ENCRYPTION_IV_SIZE      = 8 ;

static const uint32_t INTERVAL_BETWEEN_DEBUG_DUMP        = 10 ;

static std::string GXS_TUNNEL_APP_NAME = "GxsTunnels" ;

static const uint8_t GXS_TUNNEL_APP_MAJOR_VERSION = 0x01 ;
static const uint8_t GXS_TUNNEL_APP_MINOR_VERSION = 0x00 ;
static const uint8_t GXS_TUNNEL_MIN_MAJOR_VERSION = 0x01 ;
static const uint8_t GXS_TUNNEL_MIN_MINOR_VERSION = 0x00 ;
        
RsGxsTunnelService *rsGxsTunnel = NULL ;

p3GxsTunnelService::p3GxsTunnelService(RsGixs *pids) 
            : mGixs(pids), mGxsTunnelMtx("GXS tunnel")
{
	mTurtle = NULL ;
        
        // any value is fine here, even 0, since items in different RS sessions will use different AES keys.
        global_item_counter = 0;//RSRandom::random_u64() ;	
}

void p3GxsTunnelService::connectToTurtleRouter(p3turtle *tr)
{
	mTurtle = tr ;
	tr->registerTunnelService(this) ;
}

bool p3GxsTunnelService::registerClientService(uint32_t service_id,RsGxsTunnelService::RsGxsTunnelClientService *service)
{
    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

    if(mRegisteredServices.find(service_id) != mRegisteredServices.end())
    {
        std::cerr << "(EE) p3GxsTunnelService::registerClientService(): trying to register client " << std::hex << service_id << std::dec << ", which is already registered!" << std::endl;
        return false;
    }
    
    std::cerr << "p3GxsTunnelService::registerClientService(): registering client service " << std::hex << service_id << std::dec << std::endl;
    
    mRegisteredServices[service_id] = service ;
    return true ;
}

int p3GxsTunnelService::tick()
{
    static time_t last_dump = 0 ;
   std::cerr << "p3GxsTunnelService::tick()" << std::endl; 
    time_t now = time(NULL);
    
    if(now > last_dump + INTERVAL_BETWEEN_DEBUG_DUMP )
    {
        last_dump = now ;
        debug_dump() ;
    }
    
    flush() ;
}

RsServiceInfo p3GxsTunnelService::getServiceInfo()
{
    return RsServiceInfo(RS_SERVICE_TYPE_GXS_TUNNEL, 
		GXS_TUNNEL_APP_NAME,
		GXS_TUNNEL_APP_MAJOR_VERSION, 
		GXS_TUNNEL_APP_MINOR_VERSION, 
		GXS_TUNNEL_MIN_MAJOR_VERSION, 
		GXS_TUNNEL_MIN_MINOR_VERSION);
}

void p3GxsTunnelService::flush()
{
    // Flush pending DH items. This is a higher priority, so we deal with them first.
    
    std::cerr << "p3GxsTunnelService::flush() flushing pending items." << std::endl;
    {
	    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

	    while(!pendingDHItems.empty())
		    if(locked_sendClearTunnelData(pendingDHItems.front()) )
			    pendingDHItems.pop_front() ;
    }
        
    // Flush items that could not be sent, probably because of a Mutex protected zone.
    //
    {
	    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

	    while(!pendingGxsTunnelItems.empty())
		    if(locked_sendEncryptedTunnelData(pendingGxsTunnelItems.front()))
			    pendingGxsTunnelItems.pop_front() ;
		    else
			    std::cerr << "Cannot send encrypted data item to tunnel " << pendingGxsTunnelItems.front()->PeerId() << std::endl;
    }
    
    // Look at pending data item, and re-send them if necessary.
    
    {
	    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

	    time_t now = time(NULL) ;

	    for(std::map<uint64_t, GxsTunnelData>::iterator it = pendingGxsTunnelDataItems.begin();it != pendingGxsTunnelDataItems.end();++it)
		    if(now > RS_GXS_TUNNEL_DELAY_BETWEEN_RESEND + it->second.last_sending_attempt)
		    {
			    if(locked_sendEncryptedTunnelData(it->second.data_item))
			    {                    		
				    std::cerr << "  sending data item #" << std::hex << it->first << std::dec << std::endl;
				    it->second.last_sending_attempt = now ;
			    }
			    else
				    std::cerr << "  Cannot send item " << std::hex << it->first << std::dec << std::endl;
		    }
    }

    // TODO:  also sweep GXS id map and disable any ID with no virtual peer id in the list.

    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

    time_t now = time(NULL) ;

    for(std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it(_gxs_tunnel_contacts.begin());it!=_gxs_tunnel_contacts.end();++it)
    {
        if(it->second.last_contact+20+GXS_TUNNEL_KEEP_ALIVE_TIMEOUT < now && it->second.status == RS_GXS_TUNNEL_STATUS_CAN_TALK)
        {
            std::cerr << "(II) GxsTunnelService:: connexion interrupted with peer." << std::endl;
            
            it->second.status = RS_GXS_TUNNEL_STATUS_TUNNEL_DN ;
            it->second.virtual_peer_id.clear() ;

            // Also reset turtle router monitoring so as to make the tunnel handling more responsive. If we don't do that,
            // the TR will wait 60 secs for the tunnel to die, which causes a significant waiting time in the chat window.

            if(it->second.direction == RsTurtleGenericTunnelItem::DIRECTION_SERVER)
            {
                std::cerr << "(II) GxsTunnelService:: forcing new tunnel campain." << std::endl;

                mTurtle->forceReDiggTunnels( randomHashFromDestinationGxsId(it->second.to_gxs_id) );
            }
        }
        if(it->second.last_keep_alive_sent + GXS_TUNNEL_KEEP_ALIVE_TIMEOUT < now && it->second.status == RS_GXS_TUNNEL_STATUS_CAN_TALK)
        {
            RsGxsTunnelStatusItem *cs = new RsGxsTunnelStatusItem ;

            cs->flags = RS_GXS_TUNNEL_FLAG_KEEP_ALIVE;
            cs->PeerId(it->second.virtual_peer_id) ;

            // we send off-mutex to avoid deadlock.

            pendingGxsTunnelItems.push_back(cs) ;

            it->second.last_keep_alive_sent = now ;
#ifdef DEBUG_GXS_TUNNEL
            std::cerr << "(II) GxsTunnelService:: Sending keep alive packet to gxs id " << it->first << std::endl;
#endif
        }
    }
}

// In this function the PeerId is the GXS tunnel ID.

void p3GxsTunnelService::handleIncomingItem(const RsGxsTunnelId& tunnel_id,RsGxsTunnelItem *item)
{
    if(item == NULL)
	    return ;

    // We have 3 things to do:
    //
    // 1 - if it's a data item, send an ACK
    // 2 - if it's an ack item, mark the item as properly received, and remove it from the queue
    // 3 - if it's a status item, act accordingly.

    switch(item->PacketSubType())
    {

    case RS_PKT_SUBTYPE_GXS_TUNNEL_DATA:		handleRecvTunnelDataItem(tunnel_id,dynamic_cast<RsGxsTunnelDataItem*>(item)) ;
	    break ;

    case RS_PKT_SUBTYPE_GXS_TUNNEL_DATA_ACK:		handleRecvTunnelDataAckItem(tunnel_id,dynamic_cast<RsGxsTunnelDataAckItem*>(item)) ;
	    break ;

    case RS_PKT_SUBTYPE_GXS_TUNNEL_STATUS:		handleRecvStatusItem(tunnel_id,dynamic_cast<RsGxsTunnelStatusItem*>(item)) ;
	    break ;

    default:
	    std::cerr << "(EE) impossible situation. DH items should be handled at the service level" << std::endl;
    }

    delete item ;
}

void p3GxsTunnelService::handleRecvTunnelDataAckItem(const RsGxsTunnelId& id,RsGxsTunnelDataAckItem *item) 
{
    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
    
    std::cerr << "p3GxsTunnelService::handling RecvTunnelDataAckItem()" << std::endl;
    std::cerr << "  item counter = " << std::hex << item->unique_item_counter << std::dec << std::endl;
    
    // remove it from the queue.
    
    std::map<uint64_t,GxsTunnelData>::iterator it = pendingGxsTunnelDataItems.find(item->unique_item_counter) ;
    
    if(it == pendingGxsTunnelDataItems.end())
    {
        std::cerr << "  (EE) item number " << std::hex << item->unique_item_counter << " is unknown. This is unexpected." << std::endl;
        return ;
    }
    
    delete it->second.data_item ;
    pendingGxsTunnelDataItems.erase(it) ;
}

void p3GxsTunnelService::handleRecvTunnelDataItem(const RsGxsTunnelId& tunnel_id,RsGxsTunnelDataItem *item) 
{
    // imediately send an ACK for this item
    
    RsGxsTunnelDataAckItem *ackitem = new RsGxsTunnelDataAckItem ;
    
    ackitem->unique_item_counter = item->unique_item_counter ;
    ackitem->PeerId(item->PeerId());
    
    {
	    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
	    pendingGxsTunnelItems.push_back(ackitem) ;	// we use the queue that does not need an ACK, in order to avoid an infinite loop ;-)
    }
    
    // notify the client for the received data
    
    std::cerr << "p3GxsTunnelService::handleRecvTunnelDataItem()" << std::endl;
    std::cerr << "    data size  = " << item->data_size << std::endl;
    std::cerr << "    service id = " << std::hex << item->service_id << std::dec << std::endl;
    std::cerr << "    counter id = " << std::hex << item->unique_item_counter << std::dec << std::endl;
    
    RsGxsTunnelClientService *service = NULL ;
    {
	    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
	    std::map<uint32_t,RsGxsTunnelClientService *>::const_iterator it = mRegisteredServices.find(item->service_id) ;

	    if(it == mRegisteredServices.end())
	    {
		    std::cerr << "  (EE) no registered service with ID " << std::hex << item->service_id << std::dec << ". Rejecting item." << std::endl;
		    return ;
	    }
            service = it->second ;
    }
    
    service->receiveData(tunnel_id,item->data,item->data_size) ;
    
    item->data = NULL ;		// avoids deletion, since the client has the memory now
    item->data_size = 0 ;
}

void p3GxsTunnelService::handleRecvStatusItem(const RsGxsTunnelId& tunnel_id, RsGxsTunnelStatusItem *cs)
{
    if(cs->flags & RS_GXS_TUNNEL_FLAG_CLOSING_DISTANT_CONNECTION)
	    markGxsTunnelAsClosed(tunnel_id);

    // nothing more to do, because the decryption routing will update the last_contact time when decrypting.

    if(cs->flags & RS_GXS_TUNNEL_FLAG_KEEP_ALIVE)
        std::cerr << "GxsTunnelService::handleRecvGxsTunnelStatusItem(): received keep alive packet for inactive tunnel! peerId=" << cs->PeerId() << " tunnel=" << tunnel_id << std::endl;
}

bool p3GxsTunnelService::handleTunnelRequest(const RsFileHash& hash,const RsPeerId& /*peer_id*/)
{
	RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

	// look into owned GXS ids, and see if the hash corresponds to the expected hash
	//
	std::list<RsGxsId> own_id_list ;
	rsIdentity->getOwnIds(own_id_list) ;

	// extract the GXS id from the hash

	RsGxsId destination_id = destinationGxsIdFromHash(hash) ;

	// linear search. Not costly because we have typically a low number of IDs. Otherwise, this really should be avoided!

	for(std::list<RsGxsId>::const_iterator it(own_id_list.begin());it!=own_id_list.end();++it)
		if(*it == destination_id)
		{
#ifdef DEBUG_GXS_TUNNEL
			std::cerr << "GxsTunnelService::handleTunnelRequest: received tunnel request for hash " << hash << std::endl;
			std::cerr << "  answering true!" << std::endl;
#endif
			return true ;
		}

	return false ;
}

void p3GxsTunnelService::addVirtualPeer(const TurtleFileHash& hash,const TurtleVirtualPeerId& virtual_peer_id,RsTurtleGenericTunnelItem::Direction dir)
{
#ifdef DEBUG_GXS_TUNNEL
	std::cerr << "GxsTunnelService:: received new virtual peer " << virtual_peer_id << " for hash " << hash << ", dir=" << dir << std::endl;
#endif
	RsGxsId own_gxs_id ;

	{
		RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

		GxsTunnelDHInfo& dhinfo( _gxs_tunnel_virtual_peer_ids[virtual_peer_id] ) ;
		dhinfo.gxs_id.clear() ;

		if(dhinfo.dh != NULL)
			DH_free(dhinfo.dh) ;

		dhinfo.dh = NULL ;
		dhinfo.direction = dir ;
		dhinfo.hash = hash ;
		dhinfo.status = RS_GXS_TUNNEL_DH_STATUS_UNINITIALIZED ;
		dhinfo.tunnel_id.clear();

		if(dir == RsTurtleGenericTunnelItem::DIRECTION_CLIENT)	// server side
		{
			// check that a tunnel is not already working for this hash. If so, give up.

			own_gxs_id = destinationGxsIdFromHash(hash) ;
		}
		else	// client side
		{
			std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it = _gxs_tunnel_contacts.begin();
            
            		while(it != _gxs_tunnel_contacts.end() && it->second.hash != hash) ++it ;

			if(it == _gxs_tunnel_contacts.end())
			{
				std::cerr << "(EE) no pre-registered peer for hash " << hash << " on client side. This is a bug." << std::endl;
				return ;
			}

			if(it->second.status == RS_GXS_TUNNEL_STATUS_CAN_TALK)
			{
				std::cerr << "  virtual peer is for a distant chat session that is already openned and alive. Giving it up." << std::endl;
				return ;
			}

			own_gxs_id = it->second.own_gxs_id ;
		}

#ifdef DEBUG_GXS_TUNNEL
		std::cerr << "  Creating new virtual peer ID entry and empty DH session key." << std::endl;
#endif

	}

#ifdef DEBUG_GXS_TUNNEL
	std::cerr << "  Adding virtual peer " << virtual_peer_id << " for chat hash " << hash << std::endl;
#endif

	// Start a new DH session for this tunnel
	RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

	locked_restartDHSession(virtual_peer_id,own_gxs_id) ;
}

void p3GxsTunnelService::locked_restartDHSession(const RsPeerId& virtual_peer_id,const RsGxsId& own_gxs_id)
{
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "Starting new DH session." << std::endl;
#endif
    GxsTunnelDHInfo& dhinfo = _gxs_tunnel_virtual_peer_ids[virtual_peer_id] ;	// creates it, if necessary

    dhinfo.status = RS_GXS_TUNNEL_DH_STATUS_UNINITIALIZED ;
    dhinfo.own_gxs_id = own_gxs_id ;

    if(!locked_initDHSessionKey(dhinfo.dh))
    {
        std::cerr << "  (EE) Cannot start DH session. Something went wrong." << std::endl;
        return ;
    }
    dhinfo.status = RS_GXS_TUNNEL_DH_STATUS_HALF_KEY_DONE ;

    if(!locked_sendDHPublicKey(dhinfo.dh,own_gxs_id,virtual_peer_id))
        std::cerr << "  (EE) Cannot send DH public key. Something went wrong." << std::endl;
}

void p3GxsTunnelService::removeVirtualPeer(const TurtleFileHash& hash,const TurtleVirtualPeerId& virtual_peer_id)
{
    bool tunnel_dn = false ;

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService: Removing virtual peer " << virtual_peer_id << " for hash " << hash << std::endl;
#else
    /* remove unused parameter warnings */
    (void) hash;
#endif
    {
        RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

        RsGxsId gxs_id ;
        std::map<TurtleVirtualPeerId,GxsTunnelDHInfo>::iterator it = _gxs_tunnel_virtual_peer_ids.find(virtual_peer_id) ;

        if(it == _gxs_tunnel_virtual_peer_ids.end())
        {
            std::cerr << "(EE) Cannot remove virtual peer " << virtual_peer_id << ": not found in tunnel list!!" << std::endl;
            return ;
        }

        RsGxsTunnelId tunnel_id = it->second.tunnel_id ;

        if(it->second.dh != NULL)
            DH_free(it->second.dh) ;
        
        _gxs_tunnel_virtual_peer_ids.erase(it) ;

        std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it2 = _gxs_tunnel_contacts.find(tunnel_id) ;

        if(it2 == _gxs_tunnel_contacts.end())
        {
            std::cerr << "(EE) Cannot find tunnel id " << tunnel_id << " in contact list. Weird." << std::endl;
            return ;
        }
        if(it2->second.virtual_peer_id == virtual_peer_id)
        {
            it2->second.status = RS_GXS_TUNNEL_STATUS_TUNNEL_DN ;
            it2->second.virtual_peer_id.clear() ;
            tunnel_dn = true ;
        }
    }

    if(tunnel_dn)
    {
#warning we should notify the client here
        //RsServer::notify()->notifyChatStatus(ChatId(RsGxsId(virtual_peer_id)),"tunnel is down...") ;
        //RsServer::notify()->notifyPeerStatusChanged(virtual_peer_id.toStdString(),RS_STATUS_OFFLINE) ;
    }
}

void p3GxsTunnelService::receiveTurtleData(RsTurtleGenericTunnelItem *gitem,const RsFileHash& hash, const RsPeerId& virtual_peer_id,RsTurtleGenericTunnelItem::Direction direction)
{
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService::receiveTurtleData(): Received turtle data. " << std::endl;
    std::cerr << "         hash = " << hash << std::endl;
    std::cerr << "         vpid = " << virtual_peer_id << std::endl;
    std::cerr << "    acting as = " << direction << std::endl;
#else
    /* remove unused parameter warnings */
    (void) direction;
#endif

    RsTurtleGenericDataItem *item = dynamic_cast<RsTurtleGenericDataItem*>(gitem) ;

    if(item == NULL)
    {
        std::cerr << "(EE) item is not a data item. That is an error." << std::endl;
        return ;
    }
    // Call the AES crypto module
    // - the IV is the first 8 bytes of item->data_bytes

    if(item->data_size < 8)
    {
        std::cerr << "(EE) item encrypted data stream is too small: size = " << item->data_size << std::endl;
        return ;
    }
    if(*((uint64_t*)item->data_bytes) != 0)	// WTF?? we should use flags
    {
#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "  Item is encrypted." << std::endl;
#endif

        // if cannot decrypt, it means the key is wrong. We need to re-negociate a new key.

        handleEncryptedData((uint8_t*)item->data_bytes,item->data_size,hash,virtual_peer_id) ;
    }
    else
    {
#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "  Item is not encrypted." << std::endl;
#endif

        // Now try deserialise the decrypted data to make an RsItem out of it.
        //
        uint32_t pktsize = item->data_size-8;
        RsItem *citem = RsGxsTunnelSerialiser().deserialise(&((uint8_t*)item->data_bytes)[8],&pktsize) ;

        if(citem == NULL)
        {
            std::cerr << "(EE) item could not be de-serialized. That is an error." << std::endl;
            return ;
        }

        // DH key items are sent even before we know who we speak to, so the virtual peer id is used in this
        // case only.
        RsGxsTunnelDHPublicKeyItem *dhitem = dynamic_cast<RsGxsTunnelDHPublicKeyItem*>(citem) ;
        
        if(dhitem != NULL)
        {
            dhitem->PeerId(virtual_peer_id) ;
            handleRecvDHPublicKey(dhitem) ;
        }
        else
            std::cerr << "(EE) Deserialiased item has unexpected type." << std::endl;
    }
}

// This function encrypts the given data and adds a MAC and an IV into a serialised memory chunk that is then sent through the tunnel.

bool p3GxsTunnelService::handleEncryptedData(const uint8_t *data_bytes,uint32_t data_size,const TurtleFileHash& hash,const RsPeerId& virtual_peer_id)
{
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "p3GxsTunnelService::handleEncryptedDataItem()" << std::endl;
    std::cerr << "   size = " << data_size << std::endl;
    std::cerr << "   data = " << (void*)data_bytes << std::endl;
    std::cerr << "     IV = " << std::hex << *(uint64_t*)data_bytes << std::dec << std::endl;
    std::cerr << "   data = " << RsUtil::BinToHex((char*)data_bytes,data_size) ;
    std::cerr << std::endl;
#endif

    RsGxsTunnelItem *citem = NULL;
    RsGxsTunnelId tunnel_id;

    {
        RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

        uint32_t encrypted_size = data_size - GXS_TUNNEL_ENCRYPTION_IV_SIZE - GXS_TUNNEL_ENCRYPTION_HMAC_SIZE;
        uint32_t decrypted_size = RsAES::get_buffer_size(encrypted_size);
        uint8_t *encrypted_data = (uint8_t*)data_bytes+GXS_TUNNEL_ENCRYPTION_IV_SIZE+GXS_TUNNEL_ENCRYPTION_HMAC_SIZE;
        uint8_t *decrypted_data = new uint8_t[decrypted_size];
        uint8_t aes_key[GXS_TUNNEL_AES_KEY_SIZE] ;

        std::map<TurtleVirtualPeerId,GxsTunnelDHInfo>::iterator it = _gxs_tunnel_virtual_peer_ids.find(virtual_peer_id) ;

        if(it == _gxs_tunnel_virtual_peer_ids.end())
        {
            std::cerr << "(EE) item is not coming out of a registered tunnel. Weird. hash=" << hash << ", peer id = " << virtual_peer_id << std::endl;
            return true ;
        }

        tunnel_id = it->second.tunnel_id ;
        std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it2 = _gxs_tunnel_contacts.find(tunnel_id) ;

        if(it2 == _gxs_tunnel_contacts.end())
        {
            std::cerr << "(EE) no tunnel data for tunnel ID=" << tunnel_id << ". This is a bug." << std::endl;
            return true ;
        }
        memcpy(aes_key,it2->second.aes_key,GXS_TUNNEL_AES_KEY_SIZE) ;

#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "   Using IV: " << std::hex << *(uint64_t*)data_bytes << std::dec << std::endl;
        std::cerr << "   Decrypted buffer size: " << decrypted_size << std::endl;
        std::cerr << "   key  : " << RsUtil::BinToHex((char*)aes_key,GXS_TUNNEL_AES_KEY_SIZE) << std::endl;
        std::cerr << "   hmac : " << RsUtil::BinToHex((char*)data_bytes+GXS_TUNNEL_ENCRYPTION_IV_SIZE,GXS_TUNNEL_ENCRYPTION_HMAC_SIZE) << std::endl;
        std::cerr << "   data : " << RsUtil::BinToHex((char*)data_bytes,data_size) << std::endl;
#endif
        // first, check the HMAC
        
        unsigned char *hm = HMAC(EVP_sha1(),aes_key,GXS_TUNNEL_AES_KEY_SIZE,encrypted_data,encrypted_size,NULL,NULL) ;
        
        if(memcmp(hm,&data_bytes[GXS_TUNNEL_ENCRYPTION_IV_SIZE],GXS_TUNNEL_ENCRYPTION_HMAC_SIZE))
        {
            std::cerr << "(EE) packet HMAC does not match. Computed HMAC=" << RsUtil::BinToHex((char*)hm,GXS_TUNNEL_ENCRYPTION_HMAC_SIZE) << std::endl;
            std::cerr << "(EE) resetting new DH session." << std::endl;

            delete[] decrypted_data ;

            locked_restartDHSession(virtual_peer_id,it2->second.own_gxs_id) ;

            return false ;
        }

        if(!RsAES::aes_decrypt_8_16(encrypted_data,encrypted_size, aes_key,(uint8_t*)data_bytes,decrypted_data,decrypted_size))
        {
            std::cerr << "(EE) packet decryption failed." << std::endl;
            std::cerr << "(EE) resetting new DH session." << std::endl;

            delete[] decrypted_data ;

            locked_restartDHSession(virtual_peer_id,it2->second.own_gxs_id) ;

            return false ;
        }
        it2->second.status = RS_GXS_TUNNEL_STATUS_CAN_TALK ;
        it2->second.last_contact = time(NULL) ;

#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "(II) Decrypted data: size=" << decrypted_size << std::endl;
#endif

        // Now try deserialise the decrypted data to make an RsItem out of it.
        //
        citem = dynamic_cast<RsGxsTunnelItem*>(RsGxsTunnelSerialiser().deserialise(decrypted_data,&decrypted_size)) ;
        
        delete[] decrypted_data ;

        if(citem == NULL)
        {
            std::cerr << "(EE) item could not be de-serialized. That is an error." << std::endl;
            return true;
        }

        // DH key items are sent even before we know who we speak to, so the virtual peer id is used in this
        // case only.

        citem->PeerId(virtual_peer_id) ;
    }

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "(II) Setting peer id to " << citem->PeerId() << std::endl;
#endif
    handleIncomingItem(tunnel_id,citem) ; // Treats the item, and deletes it

    return true ;
}

void p3GxsTunnelService::handleRecvDHPublicKey(RsGxsTunnelDHPublicKeyItem *item)
{
    if (!item)
    {
	    std::cerr << "p3GxsTunnelService:  Received null DH public key item. This should not happen." << std::endl;
        return;
    }

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService:  Received DH public key." << std::endl;
    item->print(std::cerr, 0) ;
#endif

    // Look for the current state of the key agreement.

    TurtleVirtualPeerId vpid = item->PeerId() ;

    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

    std::map<RsPeerId,GxsTunnelDHInfo>::iterator it = _gxs_tunnel_virtual_peer_ids.find(vpid) ;

    if(it == _gxs_tunnel_virtual_peer_ids.end())
    {
        std::cerr << "  (EE) Cannot find hash in gxs_tunnel peer list!!" << std::endl;
        return ;
    }

    // Now check the signature of the DH public key item.

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "  Checking signature. " << std::endl;
#endif

    uint32_t pubkey_size = BN_num_bytes(item->public_key) ;
    unsigned char *data = (unsigned char *)malloc(pubkey_size) ;
    BN_bn2bin(item->public_key, data) ;

    RsTlvSecurityKey signature_key ;

    // We need to get the key of the sender, but if the key is not cached, we
    // need to get it first. So we let the system work for 2-3 seconds before
    // giving up. Normally this would only cause a delay for uncached keys,
    // which is rare. To force the system to cache the key, we first call for
    // getIdDetails().
    //
    RsIdentityDetails details  ;
    RsGxsId senders_id( item->signature.keyId ) ;

    for(int i=0;i<6;++i)
        if(!mGixs->getKey(senders_id,signature_key) || signature_key.keyData.bin_data == NULL)
        {
#ifdef DEBUG_GXS_TUNNEL
            std::cerr << "  Cannot get key. Waiting for caching. try " << i << "/6" << std::endl;
#endif
            usleep(500 * 1000) ;	// sleep for 500 msec.
        }
        else
            break ;

    if(signature_key.keyData.bin_data == NULL)
    {
	    std::cerr << "  (EE) Key unknown for checking signature from " << senders_id << ", can't verify signature. Using key provided in DH packet (without adding to the keyring)." << std::endl;

	    // check GXS key for defects.

	    if(!GxsSecurity::checkPublicKey(item->gxs_key))
	    {
		    std::cerr << "(SS) Security error in distant chat DH handshake: supplied key " << item->gxs_key.keyId << " is inconsistent. Refusing chat!" << std::endl;
		    return ;
	    }
	    if(item->gxs_key.keyId != item->signature.keyId)
	    {
		    std::cerr << "(SS) Security error in distant chat DH handshake: supplied key " << item->gxs_key.keyId << " is not the same than the item's signature key " << item->signature.keyId << ". Refusing chat!" << std::endl;
		    return ;
	    }

	    signature_key = item->gxs_key ;
    }

    if(!GxsSecurity::validateSignature((char*)data,pubkey_size,signature_key,item->signature))
    {
        std::cerr << "(SS) Signature was verified and it doesn't check! This is a security issue!" << std::endl;
        return ;
    }
    mGixs->timeStampKey(item->signature.keyId) ;

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "  Signature checks! Sender's ID = " << senders_id << std::endl;
    std::cerr << "  Computing AES key" << std::endl;
#endif

    if(it->second.dh == NULL)
    {
        std::cerr << "  (EE) no DH information for that peer. This is an error." << std::endl;
        return ;
    }
    if(it->second.status == RS_GXS_TUNNEL_DH_STATUS_KEY_AVAILABLE)
    {
#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "  DH Session already set for this tunnel. Re-initing a new session!" << std::endl;
#endif

        locked_restartDHSession(vpid,it->second.own_gxs_id) ;
    }

    // gets current key params. By default, should contain all null pointers.
    //
    RsGxsId own_id = it->second.own_gxs_id ;
    
    RsGxsTunnelId tunnel_id = makeGxsTunnelId(own_id,senders_id) ;
    
    it->second.tunnel_id = tunnel_id ;
    it->second.gxs_id = senders_id ;

    // Looks for the DH params. If not there yet, create them.
    //
    int size = DH_size(it->second.dh) ;
    unsigned char *key_buff = new unsigned char[size] ;

    if(size != DH_compute_key(key_buff,item->public_key,it->second.dh))
    {
        std::cerr << "  (EE) DH computation failed. Probably a bug. Error code=" << ERR_get_error() << std::endl;
        return ;
    }
    it->second.status = RS_GXS_TUNNEL_DH_STATUS_KEY_AVAILABLE ;

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "  DH key computation successed. New key in place." << std::endl;
#endif
    // make a hash of destination and source GXS ids in order to create the tunnel name
    
    GxsTunnelPeerInfo& pinfo(_gxs_tunnel_contacts[tunnel_id]) ;

    // Now hash the key buffer into a 16 bytes key.

    assert(GXS_TUNNEL_AES_KEY_SIZE <= Sha1CheckSum::SIZE_IN_BYTES) ;
    memcpy(pinfo.aes_key, RsDirUtil::sha1sum(key_buff,size).toByteArray(),GXS_TUNNEL_AES_KEY_SIZE) ;
    delete[] key_buff ;
    
    pinfo.last_contact = time(NULL) ;
    pinfo.last_keep_alive_sent = time(NULL) ;
    pinfo.status = RS_GXS_TUNNEL_STATUS_CAN_TALK ;
    pinfo.virtual_peer_id = vpid ;
    pinfo.direction = it->second.direction ;
    pinfo.own_gxs_id = own_id ;
    
    // note: the hash might still be nn initialised on server side.

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "  DH key computed. Tunnel is now secured!" << std::endl;
    std::cerr << "  Key computed: " << RsUtil::BinToHex((char*)pinfo.aes_key,16) << std::cerr << std::endl;
    std::cerr << "  Sending a ACK packet." << std::endl;
#endif

    // then we send an ACK packet to notify that the tunnel works. That's useful
    // because it makes the peer at the other end of the tunnel know that all
    // intermediate peer in the tunnel are able to transmit the data.
    // However, it is not possible here to call sendTurtleData(), without dead-locking
    // the turtle router, so we store the item is a list of items to be sent.

    RsGxsTunnelStatusItem *cs = new RsGxsTunnelStatusItem ;

    cs->flags = RS_GXS_TUNNEL_FLAG_ACK_DISTANT_CONNECTION;
    cs->PeerId(vpid) ;

    pendingGxsTunnelItems.push_back(cs) ;
}

// Note: for some obscure reason, the typedef does not work here. Looks like a compiler error. So I use the primary type.

GXSTunnelId p3GxsTunnelService::makeGxsTunnelId(const RsGxsId &own_id, const RsGxsId &distant_id) const	// creates a unique ID from two GXS ids.
{
    unsigned char mem[RsGxsId::SIZE_IN_BYTES * 2] ;
    
    // Always sort the ids, as a matter to avoid confusion between the two. Also that generates the same tunnel ID on both sides
    // which helps debugging. If the code is right this is not needed anyway.
    
    if(own_id < distant_id)
    {
	    memcpy(mem, own_id.toByteArray(), RsGxsId::SIZE_IN_BYTES) ;
	    memcpy(mem+RsGxsId::SIZE_IN_BYTES, distant_id.toByteArray(), RsGxsId::SIZE_IN_BYTES) ;
    }
    else
    {
	    memcpy(mem, distant_id.toByteArray(), RsGxsId::SIZE_IN_BYTES) ;
	    memcpy(mem+RsGxsId::SIZE_IN_BYTES, own_id.toByteArray(), RsGxsId::SIZE_IN_BYTES) ;
    }
    
    assert( RsGxsTunnelId::SIZE_IN_BYTES <= Sha1CheckSum::SIZE_IN_BYTES ) ;
    
    return RsGxsTunnelId( RsDirUtil::sha1sum(mem, 2*RsGxsId::SIZE_IN_BYTES).toByteArray() ) ;
}

bool p3GxsTunnelService::locked_sendDHPublicKey(const DH *dh,const RsGxsId& own_gxs_id,const RsPeerId& virtual_peer_id)
{
	if(dh == NULL)
	{
		std::cerr << "  (EE) DH struct is not initialised! Error." << std::endl;
		return false ;
	}

	RsGxsTunnelDHPublicKeyItem *dhitem = new RsGxsTunnelDHPublicKeyItem ;
	dhitem->public_key = BN_dup(dh->pub_key) ;

	// we should also sign the data and check the signature on the other end.
	//
	RsTlvKeySignature signature ;
	RsTlvSecurityKey  signature_key ;
	RsTlvSecurityKey  signature_key_public ;

	uint32_t error_status ;

	uint32_t size = BN_num_bytes(dhitem->public_key) ;
	unsigned char *data = (unsigned char *)malloc(size) ;
	BN_bn2bin(dhitem->public_key, data) ;

	if(!mGixs->signData((unsigned char*)data,size,own_gxs_id,signature,error_status))
	{
		switch(error_status)
		{
		case RsGixs::RS_GIXS_ERROR_KEY_NOT_AVAILABLE: std::cerr << "(EE) Key is not available. Cannot sign." << std::endl;
			break ;
		default: std::cerr << "(EE) Unknown error when signing" << std::endl;
			break ;
		}
		free(data) ;
		delete(dhitem);
		return false;
	}
	free(data) ;

	if(!mGixs->getKey(own_gxs_id,signature_key_public))
	{
		std::cerr << "  (EE) Could not retrieve own public key for ID = " << own_gxs_id << ". Giging up sending DH session params." << std::endl;
		return false ;
	}


	assert(!(signature_key_public.keyFlags & RSTLV_KEY_TYPE_FULL)) ;

	dhitem->signature = signature ;
	dhitem->gxs_key = signature_key_public ;
	dhitem->PeerId(virtual_peer_id) ;	

#ifdef DEBUG_GXS_TUNNEL
	std::cerr << "  Pushing DH session key item to pending distant messages..." << std::endl;
	dhitem->print(std::cerr, 2) ;
	std::cerr << std::endl;
#endif
	pendingDHItems.push_back(dhitem) ; // sent off-mutex to avoid deadlocking.

	return true ;
}

bool p3GxsTunnelService::locked_initDHSessionKey(DH *& dh)
{
    // We use our own DH group prime. This has been generated with command-line openssl and checked.
    
    static const std::string dh_prime_2048_hex = "B3B86A844550486C7EA459FA468D3A8EFD71139593FE1C658BBEFA9B2FC0AD2628242C2CDC2F91F5B220ED29AAC271192A7374DFA28CDDCA70252F342D0821273940344A7A6A3CB70C7897A39864309F6CAC5C7EA18020EF882693CA2C12BB211B7BA8367D5A7C7252A5B5E840C9E8F081469EBA0B98BCC3F593A4D9C4D5DF539362084F1B9581316C1F80FDAD452FD56DBC6B8ED0775F596F7BB22A3FE2B4753764221528D33DB4140DE58083DB660E3E105123FC963BFF108AC3A268B7380FFA72005A1515C371287C5706FFA6062C9AC73A9B1A6AC842C2764CDACFC85556607E86611FDF486C222E4896CDF6908F239E177ACC641FCBFF72A758D1C10CBB" ;

    if(dh != NULL)
    {
        DH_free(dh) ;
        dh = NULL ;
    }

    dh = DH_new() ;

    if(!dh)
    {
        std::cerr << "  (EE) DH_new() failed." << std::endl;
        return false ;
    }

    BN_hex2bn(&dh->p,dh_prime_2048_hex.c_str()) ;
    BN_hex2bn(&dh->g,"5") ;

    int codes = 0 ;

    if(!DH_check(dh, &codes) || codes != 0)
    {
        std::cerr << "  (EE) DH check failed!" << std::endl;
        return false ;
    }

    if(!DH_generate_key(dh))
    {
        std::cerr << "  (EE) DH generate_key() failed! Error code = " << ERR_get_error() << std::endl;
        return false ;
    }
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "  (II) DH Session key inited." << std::endl;
#endif
    return true ;
}

// Encrypts and sends the item.

bool p3GxsTunnelService::locked_sendClearTunnelData(RsGxsTunnelDHPublicKeyItem *item)
{
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService::sendClearTunnelData(): try sending item " << (void*)item << " to peer " << item->PeerId() << std::endl;
#endif

    // make a TurtleGenericData item out of it, and send it in clear.
    //
    RsTurtleGenericDataItem *gitem = new RsTurtleGenericDataItem ;

    uint32_t rssize = item->serial_size() ;

    gitem->data_size  = rssize + 8 ;
    gitem->data_bytes = malloc(rssize+8) ;

    // by convention, we use a IV of 0 for unencrypted data.
    memset(gitem->data_bytes,0,8) ;

    if(!item->serialise(&((uint8_t*)gitem->data_bytes)[8],rssize))
    {
	    std::cerr << "(EE) Could not serialise item!!!" << std::endl;
	    delete gitem ;
	    return false;
    }

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "   GxsTunnelService::sendClearTunnelData(): Sending clear data to virtual peer: " << item->PeerId() << std::endl;
    std::cerr << "     gitem->data_size = " << gitem->data_size << std::endl;
    std::cerr << "     data = " << RsUtil::BinToHex((char*)gitem->data_bytes,gitem->data_size) ;
    std::cerr << std::endl;
#endif
    mTurtle->sendTurtleData(item->PeerId(),gitem) ;
    
    return true ;
}

// Sends this item using secured/authenticated method, thx to the establshed cryptographic channel.

bool p3GxsTunnelService::locked_sendEncryptedTunnelData(RsGxsTunnelItem *item)
{
    uint32_t rssize = item->serial_size();
    RsTemporaryMemory buff(rssize) ;

    if(!item->serialise(buff,rssize))
    {
	    std::cerr << "(EE) GxsTunnelService::sendEncryptedTunnelData(): Could not serialise item!" << std::endl;
	    return false;
    }

    uint8_t aes_key[GXS_TUNNEL_AES_KEY_SIZE] ;
    uint64_t IV = 0;

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "Sending encrypted data to tunnel wuth vpid " << item->PeerId() << std::endl;
#endif
    TurtleVirtualPeerId vpid = item->PeerId() ;
    
    std::map<TurtleVirtualPeerId,GxsTunnelDHInfo>::const_iterator it2 = _gxs_tunnel_virtual_peer_ids.find(vpid) ;
    if(it2 == _gxs_tunnel_virtual_peer_ids.end())
    {
        std::cerr << "(EE) no virtual peer " << vpid << ". Something's wrong!" << std::endl;
        return false;
    }
        
    std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it = _gxs_tunnel_contacts.find(it2->second.tunnel_id) ;

    if(it == _gxs_tunnel_contacts.end())
    {
        std::cerr << "(EE) Cannot find contact key info for virtual peer id " << vpid << ". Cannot send message!" << std::endl;
        return false;
    }
    if(it->second.status != RS_GXS_TUNNEL_STATUS_CAN_TALK)
    {
        std::cerr << "(EE) Cannot talk to vpid " << vpid << ". Tunnel status is: " << it->second.status << std::endl;
        return false;
    }

    memcpy(aes_key,it->second.aes_key,GXS_TUNNEL_AES_KEY_SIZE) ;
    RsPeerId virtual_peer_id = it->second.virtual_peer_id ;

    while(IV == 0) IV = RSRandom::random_u64() ; // make a random 8 bytes IV, that is not 0

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService::sendEncryptedTunnelData(): tunnel found. Encrypting data." << std::endl;
#endif

    // Now encrypt this data using AES.
    //
    uint32_t encrypted_size = RsAES::get_buffer_size(rssize);
    RsTemporaryMemory encrypted_data(encrypted_size) ;

    if(!RsAES::aes_crypt_8_16(buff,rssize,aes_key,(uint8_t*)&IV,encrypted_data,encrypted_size))
    {
        std::cerr << "(EE) packet encryption failed." << std::endl;
        delete[] encrypted_data ;
        return false;
    }

    // make a TurtleGenericData item out of it:
    //
    RsTurtleGenericDataItem *gitem = new RsTurtleGenericDataItem ;

    gitem->data_size  = encrypted_size + GXS_TUNNEL_ENCRYPTION_IV_SIZE + GXS_TUNNEL_ENCRYPTION_HMAC_SIZE ;
    gitem->data_bytes = malloc(gitem->data_size) ;

    memcpy(& ((uint8_t*)gitem->data_bytes)[0]                                       ,&IV,8) ;

    unsigned int md_len = GXS_TUNNEL_ENCRYPTION_HMAC_SIZE ;
    HMAC(EVP_sha1(),aes_key,GXS_TUNNEL_AES_KEY_SIZE,encrypted_data,encrypted_size,&(((uint8_t*)gitem->data_bytes)[GXS_TUNNEL_ENCRYPTION_IV_SIZE]),&md_len) ;
    
    memcpy(& (((uint8_t*)gitem->data_bytes)[GXS_TUNNEL_ENCRYPTION_HMAC_SIZE+GXS_TUNNEL_ENCRYPTION_IV_SIZE]),encrypted_data,encrypted_size) ;
    
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "   Using  IV: " << std::hex << IV << std::dec << std::endl;
    std::cerr << "   Using Key: " << RsUtil::BinToHex((char*)aes_key,GXS_TUNNEL_AES_KEY_SIZE) ; std::cerr << std::endl;
    std::cerr << "        hmac: " << RsUtil::BinToHex((char*)gitem->data_bytes,GXS_TUNNEL_ENCRYPTION_HMAC_SIZE) ;
#endif
#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "GxsTunnelService::sendEncryptedTunnelData(): Sending encrypted data to virtual peer: " << virtual_peer_id << std::endl;
    std::cerr << "   gitem->data_size = " << gitem->data_size << std::endl;
    std::cerr << "    serialised data = " << RsUtil::BinToHex((char*)gitem->data_bytes,gitem->data_size) ;
    std::cerr << std::endl;
#endif

    mTurtle->sendTurtleData(virtual_peer_id,gitem) ;
    
    return true ;
}

bool p3GxsTunnelService::requestSecuredTunnel(const RsGxsId& to_gxs_id, const RsGxsId& from_gxs_id, RsGxsTunnelId &tunnel_id, uint32_t& error_code)
{
    // should be a parameter.
	
    std::list<RsGxsId> lst ;
    mGixs->getOwnIds(lst) ;

    bool found = false ;
    for(std::list<RsGxsId>::const_iterator it = lst.begin();it!=lst.end();++it)
        if(*it == from_gxs_id)
        {
            found=true;
            break ;
        }

    if(!found)
    {
        std::cerr << "  (EE) Cannot start distant chat, since GXS id " << from_gxs_id << " is not available." << std::endl;
        error_code = RS_GXS_TUNNEL_ERROR_UNKNOWN_GXS_ID ;
        return false ;
    }
    RsGxsId own_gxs_id = from_gxs_id ;

    startClientGxsTunnelConnection(to_gxs_id,own_gxs_id,tunnel_id) ;

    error_code = RS_GXS_TUNNEL_ERROR_NO_ERROR ;

    return true ;
}

bool p3GxsTunnelService::sendData(const RsGxsTunnelId &tunnel_id, uint32_t service_id, const uint8_t *data, uint32_t size)
{
    // make sure that the tunnel ID is registered.
    
    std::cerr << "p3GxsTunnelService::sendData()" << std::endl;
    std::cerr << "  tunnel id : " << tunnel_id << std::endl;
    std::cerr << "  data size : " << size << std::endl;
    std::cerr << "  service id: " << std::hex << service_id << std::dec << std::endl;
    
    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
        
    std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;
    
    if(it == _gxs_tunnel_contacts.end())
    {
        std::cerr << "  (EE) no tunnel known with this ID. Sorry!" << std::endl;
        return false ;
    }
    
    // make sure the service is registered.
    
    if(mRegisteredServices.find(service_id) == mRegisteredServices.end())
    {
        std::cerr << "  (EE) no service registered with this ID. Please call rsGxsTunnel->registerClientService() at some point." << std::endl;
        return false ;
    }
    
    std::cerr << "  verifications fine! Storing in out queue with:" << std::endl;
    
    RsGxsTunnelDataItem *item = new RsGxsTunnelDataItem ;
            
    item->unique_item_counter = global_item_counter++;		// this allows to make the item unique
    item->flags = 0;						// not used yet.
    item->service_id = service_id;
    item->data_size = size;					// encrypted data size
    item->data = (uint8_t*)malloc(size);			// encrypted data
    memcpy(item->data,data,size) ;
    
    GxsTunnelData& tdata( pendingGxsTunnelDataItems[item->unique_item_counter] ) ;
    
    tdata.data_item = item ;
    tdata.last_sending_attempt = 0 ;	// never sent until now
    
    std::cerr << "  counter id : " << std::hex << item->unique_item_counter << std::dec << std::endl;
    
    return true ;
}


void p3GxsTunnelService::startClientGxsTunnelConnection(const RsGxsId& to_gxs_id,const RsGxsId& from_gxs_id,RsGxsTunnelId& tunnel_id)
{
    // compute a random hash for that pair, and init the DH session for it so that we can recognise it when we get the virtual peer for it.
    
    RsFileHash hash = randomHashFromDestinationGxsId(to_gxs_id) ;
    
    tunnel_id = makeGxsTunnelId(from_gxs_id,to_gxs_id) ;
    
    {
        RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

        if(_gxs_tunnel_contacts.find(tunnel_id) != _gxs_tunnel_contacts.end())
        {
            std::cerr << "GxsTunnelService:: asking GXS tunnel for a configuration that already exits.Ignoring." << std::endl;
            return ;
        }
    }

    GxsTunnelPeerInfo info ;

    time_t now = time(NULL) ;

    info.last_contact = now ;
    info.last_keep_alive_sent = now ;
    info.status = RS_GXS_TUNNEL_STATUS_TUNNEL_DN ;
    info.own_gxs_id = from_gxs_id ;
    info.to_gxs_id = to_gxs_id ;
    info.hash = hash ;
    info.direction = RsTurtleGenericTunnelItem::DIRECTION_SERVER ;
    info.virtual_peer_id.clear();

    memset(info.aes_key,0,GXS_TUNNEL_AES_KEY_SIZE) ;

    {
        RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
        
        _gxs_tunnel_contacts[tunnel_id] = info ;
    }

#ifdef DEBUG_GXS_TUNNEL
    std::cerr << "Starting distant chat to " << to_gxs_id << ", hash = " << hash << ", from " << from_gxs_id << std::endl;
    std::cerr << "Asking turtle router to monitor tunnels for hash " << hash << std::endl;
#endif

    // Now ask the turtle router to manage a tunnel for that hash.
    
    mTurtle->monitorTunnels(hash,this,false) ;
}

TurtleFileHash p3GxsTunnelService::randomHashFromDestinationGxsId(const RsGxsId& destination)
{
    // This is in prevision for the "secured GXS tunnel" service, which will need a service ID to register,
    // just like GRouter does.

    static const uint32_t client = RS_SERVICE_TYPE_GXS_TUNNEL ;

    assert(  destination.SIZE_IN_BYTES == 16) ;
    assert(Sha1CheckSum::SIZE_IN_BYTES == 20) ;

    uint8_t bytes[20] ;
    memcpy(bytes,destination.toByteArray(),16) ;
    
    RAND_bytes(&bytes[16],4) ;	// fill the last bytes with random crap. Very important to allow tunnels from different sources and statistically avoid collisions.

    // We could rehash this, with a secret key to get a HMAC. That would allow to publish secret distant chat
    // passphrases. I'll do this later if needed.

    return Sha1CheckSum(bytes) ;
}

RsGxsId p3GxsTunnelService::destinationGxsIdFromHash(const TurtleFileHash& sum)
{
    assert(     RsGxsId::SIZE_IN_BYTES == 16) ;
    assert(Sha1CheckSum::SIZE_IN_BYTES == 20) ;

    return RsGxsId(sum.toByteArray());// takes the first 16 bytes
}

bool p3GxsTunnelService::getTunnelInfo(const RsGxsTunnelId& tunnel_id,GxsTunnelInfo& info)
{
    RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

    std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;

    if(it == _gxs_tunnel_contacts.end())
	    return false ;

    info.destination_gxs_id = it->second.to_gxs_id;     
    info.source_gxs_id      = it->second.own_gxs_id;	
    info.tunnel_status      = it->second.status;	          
#warning data missing here
    info.total_size_sent    = 0;	         
    info.total_size_received= 0;	  

    // Data packets

    info.pending_data_packets = 0;     
    info.total_data_packets_sent=0 ;     
    info.total_data_packets_received=0 ;

    return true ;
}

bool p3GxsTunnelService::getTunnelStatus(const RsGxsTunnelId& tunnel_id,uint32_t& status)
{
    RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

    std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;

    if(it != _gxs_tunnel_contacts.end())
    {
        status = it->second.status ;

        return true ;
    }

    status = RS_GXS_TUNNEL_STATUS_UNKNOWN ;

    return false ;
}

bool p3GxsTunnelService::closeExistingTunnel(const RsGxsTunnelId& tunnel_id)
{
	// two cases: 
	// 	- client needs to stop asking for tunnels => remove the hash from the list of tunnelled files
	// 	- server needs to only close the window and let the tunnel die. But the window should only open 
	// 	  if a message arrives.

	TurtleFileHash hash ;
	TurtleVirtualPeerId vpid ;
	{
		RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
		std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;

		if(it == _gxs_tunnel_contacts.end())
		{
			std::cerr << "(EE) Cannot close distant tunnel connection. No connection openned for tunnel id " << tunnel_id << std::endl;

			// We cannot stop tunnels, since their peer id is lost. Anyway, they'll die of starving.

			return false ;
		}
		vpid = it->second.virtual_peer_id ;

		if(it->second.direction == RsTurtleGenericTunnelItem::DIRECTION_CLIENT) 	// nothing more to do for server side.
			return true ;

		std::map<TurtleVirtualPeerId, GxsTunnelDHInfo>::const_iterator it2 = _gxs_tunnel_virtual_peer_ids.find(vpid) ;

		if(it2 != _gxs_tunnel_virtual_peer_ids.end())
			hash = it2->second.hash ;
	}

	// send a status item saying that we're closing the connection
#ifdef DEBUG_GXS_TUNNEL
	std::cerr << "  Sending a ACK to close the tunnel since we're managing it. tunnel id=." << tunnel_id << std::endl;
#endif

	RsGxsTunnelStatusItem *cs = new RsGxsTunnelStatusItem ;

	cs->flags = RS_GXS_TUNNEL_FLAG_CLOSING_DISTANT_CONNECTION;
	cs->PeerId(vpid) ;

	locked_sendEncryptedTunnelData(cs) ;	// that needs to be done off-mutex and before we close the tunnel also ignoring failure.

#ifdef DEBUG_GXS_TUNNEL
	std::cerr << "  This is client side. Stopping tunnel manageement for tunnel_id " << tunnel_id << std::endl;
#endif
	mTurtle->stopMonitoringTunnels( hash ) ;	// still valid if the hash is null
	{
		RsStackMutex stack(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
		std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;

		if(it == _gxs_tunnel_contacts.end())		// server side. Nothing to do.
		{
			std::cerr << "(EE) Cannot close chat associated to tunnel id " << tunnel_id << ": not found." << std::endl;
			return false ;
		}

		_gxs_tunnel_contacts.erase(it) ;

		// GxsTunnelService::removeVirtualPeerId() will be called by the turtle service.
	}
	return true ;
}

void p3GxsTunnelService::markGxsTunnelAsClosed(const RsGxsTunnelId& tunnel_id)
{
    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/

        std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::iterator it = _gxs_tunnel_contacts.find(tunnel_id) ;

        if(it == _gxs_tunnel_contacts.end())
        {
            std::cerr << "(EE) Cannot mark distant chat connection as closed. No connection openned for tunnel id " << tunnel_id << ". Unexpected situation." << std::endl;
            return ;
        }

    if(it->second.direction == RsTurtleGenericDataItem::DIRECTION_CLIENT)
    {
#ifdef DEBUG_GXS_TUNNEL
        std::cerr << "  This is server side. Marking distant chat as remotely closed for tunnel id " << tunnel_id << std::endl;
#endif
        it->second.status = RS_GXS_TUNNEL_STATUS_REMOTELY_CLOSED ;
    }
}

void p3GxsTunnelService::debug_dump()
{
    RS_STACK_MUTEX(mGxsTunnelMtx); /********** STACK LOCKED MTX ******/
    
    time_t now = time(NULL) ;
    
    std::cerr << "p3GxsTunnelService::debug_dump()" << std::endl;
    std::cerr << "  Registered client services: " << std::endl;
    
    for(std::map<uint32_t,RsGxsTunnelService::RsGxsTunnelClientService*>::const_iterator it=mRegisteredServices.begin();it!=mRegisteredServices.end();++it)
        std::cerr << std::hex << "    " << it->first << " - " << (void*)it->second << std::dec << std::endl;
        
    std::cerr << "  Active tunnels" << std::endl;
                 
    for(std::map<RsGxsTunnelId,GxsTunnelPeerInfo>::const_iterator it=_gxs_tunnel_contacts.begin();it!=_gxs_tunnel_contacts.end();++it)
        std::cerr << "    tunnel_id=" << it->first << " vpid=" << it->second.virtual_peer_id << " status=" << it->second.status << " direction=" << it->second.direction << " last_contact=" << (now-it->second.last_contact) <<" secs ago. Last_keep_alive_sent:" << (now - it->second.last_keep_alive_sent) << " secs ago." << std::endl; 
    
    std::cerr << "  Virtual peers:" << std::endl;
    
    for(std::map<TurtleVirtualPeerId,GxsTunnelDHInfo>::const_iterator it=_gxs_tunnel_virtual_peer_ids.begin();it!=_gxs_tunnel_virtual_peer_ids.end();++it)
        std::cerr << "    vpid=" << it->first << " to=" << it->second.gxs_id << " from=" << it->second.own_gxs_id << " tunnel_id=" << it->second.tunnel_id << " status=" << it->second.status << " direction=" << it->second.direction << " hash=" << it->second.hash << std::endl;
}
















