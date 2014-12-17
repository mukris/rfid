/*
 * FreeRTOS+TCP Labs Build 141019 (C) 2014 Real Time Engineers ltd.
 * Authors include Hein Tibosch and Richard Barry
 *
 *******************************************************************************
 ***** NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ***
 ***                                                                         ***
 ***                                                                         ***
 ***   FREERTOS+TCP IS STILL IN THE LAB:                                     ***
 ***                                                                         ***
 ***   This product is functional and is already being used in commercial    ***
 ***   products.  Be aware however that we are still refining its design,    ***
 ***   the source code does not yet fully conform to the strict coding and   ***
 ***   style standards mandated by Real Time Engineers ltd., and the         ***
 ***   documentation and testing is not necessarily complete.                ***
 ***                                                                         ***
 ***   PLEASE REPORT EXPERIENCES USING THE SUPPORT RESOURCES FOUND ON THE    ***
 ***   URL: http://www.FreeRTOS.org/contact  Active early adopters may, at   ***
 ***   the sole discretion of Real Time Engineers Ltd., be offered versions  ***
 ***   under a license other than that described below.                      ***
 ***                                                                         ***
 ***                                                                         ***
 ***** NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ******* NOTE ***
 *******************************************************************************
 *
 * - Open source licensing -
 * While FreeRTOS+TCP is in the lab it is provided only under version two of the
 * GNU General Public License (GPL) (which is different to the standard FreeRTOS
 * license).  FreeRTOS+TCP is free to download, use and distribute under the
 * terms of that license provided the copyright notice and this text are not
 * altered or removed from the source files.  The GPL V2 text is available on
 * the gnu.org web site, and on the following
 * URL: http://www.FreeRTOS.org/gpl-2.0.txt.  Active early adopters may, and
 * solely at the discretion of Real Time Engineers Ltd., be offered versions
 * under a license other then the GPL.
 *
 * FreeRTOS+TCP is distributed in the hope that it will be useful.  You cannot
 * use FreeRTOS+TCP unless you agree that you use the software 'as is'.
 * FreeRTOS+TCP is provided WITHOUT ANY WARRANTY; without even the implied
 * warranties of NON-INFRINGEMENT, MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. Real Time Engineers Ltd. disclaims all conditions and terms, be they
 * implied, expressed, or statutory.
 *
 * 1 tab == 4 spaces!
 *
 * http://www.FreeRTOS.org
 * http://www.FreeRTOS.org/udp
 *
 */

/* Standard includes. */
#include <stdint.h>
#include <stdio.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_ARP.h"
#include "FreeRTOS_UDP_IP.h"
#include "FreeRTOS_DHCP.h"
#if( ipconfigUSE_LLMNR == 1 )
	#include "FreeRTOS_DNS.h"
#endif /* ipconfigUSE_LLMNR */
#include "NetworkInterface.h"
#include "NetworkBufferManagement.h"


/* When the age of an entry in the ARP table reaches this value (it counts down
to zero, so this is an old entry) an ARP request will be sent to see if the
entry is still valid and can therefore be refreshed. */
#define arpMAX_ARP_AGE_BEFORE_NEW_ARP_REQUEST		( 3 )

/* The time between gratuitous ARPs. */
#define arpGRATUITOUS_ARP_PERIOD					( 20000 / portTICK_PERIOD_MS )

/*-----------------------------------------------------------*/

/*
 * Lookup an MAC address in the ARP cache from the IP address.
 */
static eARPLookupResult_t prvCacheLookup( uint32_t ulAddressToLookup, xMACAddress_t * const pxMACAddress );

/*-----------------------------------------------------------*/

/* The ARP cache. */
static xARPCacheRow_t xARPCache[ ipconfigARP_CACHE_ENTRIES ];

/* The time at which the last gratuitous ARP was sent.  Gratuitous ARPs are used
to ensure ARP tables are up to date and to detect IP address conflicts. */
static TickType_t xLastGratuitousARPTime = 0;

/* Part of the Ethernet and ARP headers are always constant when sending an IPv4
ARP packet.  This array defines the constant parts, allowing this part of the
packet to be filled in using a simple memcpy() instead of individual writes. */
static const uint8_t xDefaultPartARPPacketHeader[] =
{
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 	/* Ethernet destination address. */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 	/* Ethernet source address. */
	0x08, 0x06, 							/* Ethernet frame type (ipARP_TYPE). */
	0x00, 0x01, 							/* usHardwareType (ipARP_HARDWARE_TYPE_ETHERNET). */
	0x08, 0x00,								/* usProtocolType. */
	ipMAC_ADDRESS_LENGTH_BYTES, 			/* ucHardwareAddressLength. */
	ipIP_ADDRESS_LENGTH_BYTES, 				/* ucProtocolAddressLength. */
	0x00, 0x01, 							/* usOperation (ipARP_REQUEST). */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 	/* xSenderHardwareAddress. */
	0x00, 0x00, 0x00, 0x00, 				/* ulSenderProtocolAddress. */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00 		/* xTargetHardwareAddress. */
};

/*-----------------------------------------------------------*/

eFrameProcessingResult_t eARPProcessPacket( xARPPacket_t * const pxARPFrame )
{
eFrameProcessingResult_t eReturn = eReleaseBuffer;
xARPHeader_t *pxARPHeader;

	pxARPHeader = &( pxARPFrame->xARPHeader );

	traceARP_PACKET_RECEIVED();

	/* Sanity check the protocol type.  Don't do anything if the local IP
	address is zero because that means a DHCP request has not completed. */
	if( ( pxARPHeader->usProtocolType == ipARP_PROTOCOL_TYPE ) && ( *ipLOCAL_IP_ADDRESS_POINTER != 0UL ) )
	{
		switch( pxARPHeader->usOperation )
		{
			case ipARP_REQUEST	:
				/* The packet contained an ARP request.  Was it for the IP
				address of the node running this code? */
				if( pxARPHeader->ulTargetProtocolAddress == *ipLOCAL_IP_ADDRESS_POINTER )
				{
					iptraceSENDING_ARP_REPLY( pxARPHeader->ulSenderProtocolAddress );

					/* The request is for the address of this node.  Add the
					entry into the ARP cache, or refresh the entry if it
					already exists. */
					vARPRefreshCacheEntry( &( pxARPHeader->xSenderHardwareAddress ), pxARPHeader->ulSenderProtocolAddress );

					/* Generate a reply payload in the same buffer. */
					pxARPHeader->usOperation = ipARP_REPLY;
					if( pxARPHeader->ulTargetProtocolAddress == pxARPHeader->ulSenderProtocolAddress )
					{
						/* A double IP address is detected! */
						/* Give the sources MAC address the value of the broadcast address, will be swapped later */
						memcpy( pxARPFrame->xEthernetHeader.xSourceAddress.ucBytes, xBroadcastMACAddress.ucBytes, sizeof( xBroadcastMACAddress ) );
						memset( pxARPHeader->xTargetHardwareAddress.ucBytes, '\0', sizeof( xMACAddress_t ) );
						pxARPHeader->ulTargetProtocolAddress = 0UL;
					}
					else
					{
						memcpy( pxARPHeader->xTargetHardwareAddress.ucBytes, pxARPHeader->xSenderHardwareAddress.ucBytes, sizeof( xMACAddress_t ) );
						pxARPHeader->ulTargetProtocolAddress = pxARPHeader->ulSenderProtocolAddress;
					}
					memcpy( pxARPHeader->xSenderHardwareAddress.ucBytes, ( void * ) ipLOCAL_MAC_ADDRESS, sizeof( xMACAddress_t ) );
					pxARPHeader->ulSenderProtocolAddress = *ipLOCAL_IP_ADDRESS_POINTER;

					eReturn = eReturnEthernetFrame;
				}
				break;

			case ipARP_REPLY :
				iptracePROCESSING_RECEIVED_ARP_REPLY( pxARPHeader->ulTargetProtocolAddress );
				vARPRefreshCacheEntry( &( pxARPHeader->xSenderHardwareAddress ), pxARPHeader->ulSenderProtocolAddress );
				break;

			default :
				/* Invalid. */
				break;
		}
	}

	return eReturn;
}

/*-----------------------------------------------------------*/

#if( ipconfigUSE_ARP_REMOVE_ENTRY != 0 )
	uint32_t ulARPRemoveCacheEntryByMac( const xMACAddress_t * pxMACAddress )
	{
	BaseType_t x;
	uint32_t lResult = 0;

		/* For each entry in the ARP cache table. */
		for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
		{
			if( ( memcmp( xARPCache[ x ].xMACAddress.ucBytes, pxMACAddress->ucBytes, sizeof( pxMACAddress->ucBytes ) ) == 0 ) )
			{
				lResult = xARPCache[ x ].ulIPAddress;
				memset( &xARPCache[ x ], '\0', sizeof( xARPCache[ x ] ) );
				break;
			}
		}
		return lResult;
	}
#endif	/* ipconfigUSE_ARP_REMOVE_ENTRY != 0 */

/*-----------------------------------------------------------*/

void vARPRefreshCacheEntry( const xMACAddress_t * pxMACAddress, const uint32_t ulIPAddress )
{
BaseType_t x, xIpEntry = -1, xMacEntry = -1, xUseEntry = 0;
uint8_t ucMinAgeFound = 0U;

	#if( ipconfigARP_STORES_REMOTE_ADDRESSES == 0 )
		/* Only process the IP address if it is on the local network. */
		if( ( ulIPAddress & xNetworkAddressing.ulNetMask ) == ( ( *ipLOCAL_IP_ADDRESS_POINTER ) & xNetworkAddressing.ulNetMask ) )
	#else
		/* If ipconfigARP_STORES_REMOTE_ADDRESSES is non-zero, IP addresses with
		a different netmask will also be stored.  After when replying to a UDP
		message from a different netmask, the IP address can be looped up and a
		reply sent.  This option is useful for systems with multiple gateways,
		the reply will surely arrive.  If ipconfigARP_STORES_REMOTE_ADDRESSES is
		zero the the gateway address is the only option. */
		if( pdTRUE )
	#endif
	{
		/* Start with the maximum possible number. */
		ucMinAgeFound--;

		/* For each entry in the ARP cache table. */
		for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
		{
			/* Does this line in the cache table hold an entry for the IP
			address	being queried? */
			if( xARPCache[ x ].ulIPAddress == ulIPAddress )
			{
				if( pxMACAddress == NULL )
				{
					/* In case the parameter pxMACAddress is NULL, an entry will be reserved to
					indicate that there is an outstanding ARP request, This entry will have
					"ucValid == pdFALSE". */
					xIpEntry = x;
					break;
				}

				/* See if the MAC-address also matches. */
				if( memcmp( xARPCache[ x ].xMACAddress.ucBytes, pxMACAddress->ucBytes, sizeof( pxMACAddress->ucBytes ) ) == 0 )
				{
					/* This function will be called for each received packet
					As this is by far the most common path the coding standard
					is relaxed in this case and a return is permitted as an
					optimisation. */
					xARPCache[ x ].ucAge = ipconfigMAX_ARP_AGE;
					xARPCache[ x ].ucValid = pdTRUE;
					return;
				}

				/* Found an entry containing ulIPAddress, but the MAC address
				doesn't match.  Might be an entry with ucValid=pdFALSE, waiting
				for an ARP reply.  Still want to see if there is match with the
				given MAC address.ucBytes.  If found, either of the two entries
				must be cleared. */
				xIpEntry = x;
			}
			else if( ( pxMACAddress != NULL ) && ( memcmp( xARPCache[ x ].xMACAddress.ucBytes, pxMACAddress->ucBytes, sizeof( pxMACAddress->ucBytes ) ) == 0 ) )
			{
				/* Found an entry with the given MAC-address, but the IP-address
				is different.  Continue looping to find a possible match with
				ulIPAddress. */
	#if( ipconfigARP_STORES_REMOTE_ADDRESSES != 0 )
				/* If ARP stores the MAC address of IP addresses outside the
				network, than the MAC address of the gateway should not be
				overwritten. */
				BaseType_t bIsLocal[ 2 ];
				bIsLocal[ 0 ] = ( ( xARPCache[ x ].ulIPAddress & xNetworkAddressing.ulNetMask ) == ( ( *ipLOCAL_IP_ADDRESS_POINTER ) & xNetworkAddressing.ulNetMask ) );
				bIsLocal[ 1 ] = ( ( ulIPAddress & xNetworkAddressing.ulNetMask ) == ( ( *ipLOCAL_IP_ADDRESS_POINTER ) & xNetworkAddressing.ulNetMask ) );
				if( bIsLocal[ 0 ] == bIsLocal[ 1 ] )
				{
					xMacEntry = x;
				}
	#else
				xMacEntry = x;
	#endif
			}
			/* _HT_
			Shouldn't we test for xARPCache[ x ].ucValid == pdFALSE here ? */
			else if( xARPCache[ x ].ucAge < ucMinAgeFound )
			{
				/* As the table is traversed, remember the table row that
				contains the oldest entry (the lowest age count, as ages are
				decremented to zero) so the row can be re-used if this function
				needs to add an entry that does not already exist. */
				ucMinAgeFound = xARPCache[ x ].ucAge;
				xUseEntry = x;
			}
		}

		if( xMacEntry >= 0 )
		{
			xUseEntry = xMacEntry;

			if( xIpEntry >= 0 )
			{
				/* Both the MAC address as well as the IP address were found in
				different locations: clear the entry which matches the
				IP-address */
				memset( &xARPCache[ xIpEntry ], '\0', sizeof( xARPCache[ xIpEntry ] ) );
			}
		}
		else if( xIpEntry >= 0 )
		{
			/* An entry containing the IP-address was found, but it had a different MAC address */
			xUseEntry = xIpEntry;
		}

		/* If the entry was not found, we use the oldest entry and set the IPaddress */
		xARPCache[ xUseEntry ].ulIPAddress = ulIPAddress;

		if( pxMACAddress != NULL )
		{
			memcpy( xARPCache[ xUseEntry ].xMACAddress.ucBytes, pxMACAddress->ucBytes, sizeof( pxMACAddress->ucBytes ) );

			iptraceARP_TABLE_ENTRY_CREATED( ulIPAddress, (*pxMACAddress) );
			/* And this entry does not need immediate attention */
			xARPCache[ xUseEntry ].ucAge = ipconfigMAX_ARP_AGE;
			xARPCache[ xUseEntry ].ucValid = pdTRUE;
		}
		else if( xIpEntry < 0 )
		{
			xARPCache[ xUseEntry ].ucAge = ipconfigMAX_ARP_RETRANSMISSIONS;
			xARPCache[ xUseEntry ].ucValid = pdFALSE;
		}
	}
}
/*-----------------------------------------------------------*/

#if( ipconfigUSE_ARP_REVERSED_LOOKUP == 1 )
	eARPLookupResult_t eARPGetCacheEntryByMac( xMACAddress_t * const pxMACAddress, uint32_t *pulIPAddress )
	{
	BaseType_t x;
	eARPLookupResult_t eReturn = eARPCacheMiss;

		/* Loop through each entry in the ARP cache. */
		for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
		{
			/* Does this row in the ARP cache table hold an entry for the MAC
			address being searched? */
			if( memcmp( pxMACAddress->ucBytes, xARPCache[ x ].xMACAddress.ucBytes, sizeof( xMACAddress_t ) ) == 0 )
			{
				*pulIPAddress = xARPCache[ x ].ulIPAddress;
				eReturn = eARPCacheHit;
				break;
			}
		}

		return eReturn;
	}
#endif /* ipconfigUSE_ARP_REVERSED_LOOKUP */

/*-----------------------------------------------------------*/

eARPLookupResult_t eARPGetCacheEntry( uint32_t *pulIPAddress, xMACAddress_t * const pxMACAddress )
{
eARPLookupResult_t eReturn;
uint32_t ulAddressToLookup;

#if( ipconfigUSE_LLMNR == 1 )
	if( *pulIPAddress == ipLLMNR_IP_ADDR )	/* Is in network byte order */
	{
		/* The LLMNR IP-address has a fixed virtual MAC address */
		memcpy( pxMACAddress->ucBytes, xLLMNR_MacAdress.ucBytes, sizeof( xMACAddress_t ) );
		eReturn = eARPCacheHit;
	}
	else
#endif
	if( ( *pulIPAddress == ipBROADCAST_IP_ADDRESS ) ||	/* Is it the general broadcast address 255.255.255.255 ? */
		( *pulIPAddress == xNetworkAddressing.ulBroadcastAddress ) )/* Or a local broadcast address 192.168.1.255 ? */
	{
		/* This is a broadcast so uses the broadcast MAC address. */
		memcpy( pxMACAddress->ucBytes, xBroadcastMACAddress.ucBytes, sizeof( xMACAddress_t ) );
		eReturn = eARPCacheHit;
	}
	else if( *ipLOCAL_IP_ADDRESS_POINTER == 0UL )
	{
		/* The IP address has not yet been assigned, so there is nothing that
		can be done. */
		eReturn = eCantSendPacket;
	}
	else
	{
		eReturn = eARPCacheMiss;
		if( ( *pulIPAddress & xNetworkAddressing.ulNetMask ) != ( ( *ipLOCAL_IP_ADDRESS_POINTER ) & xNetworkAddressing.ulNetMask ) )
		{
			#if( ipconfigARP_STORES_REMOTE_ADDRESSES == 1 )
			eReturn = prvCacheLookup( *pulIPAddress, pxMACAddress );

			if( eReturn == eARPCacheHit )
			{
				/* The stack is configured to store 'remote IP addresses', i.e. addresses
				belonging to a different the netmask.  prvCacheLookup() returned a hit, so
				the MAC address is known */
			}
			else
			#endif
			{
				/* The IP address is off the local network, so look up the
				hardware address of the router, if any. */
				ulAddressToLookup = xNetworkAddressing.ulGatewayAddress;
			}
		}
		else
		{
			/* The IP address is on the local network, so lookup the requested
			IP address directly. */
			ulAddressToLookup = *pulIPAddress;
		}

		if( eReturn == eARPCacheMiss )
		{
			if( ulAddressToLookup == 0UL )
			{
				/* The address is not on the local network, and there is not a
				router. */
				eReturn = eCantSendPacket;
			}
			else
			{
				eReturn = prvCacheLookup( ulAddressToLookup, pxMACAddress );

				if( eReturn == eARPCacheMiss )
				{
					/* It might be that the ARP has to go to the gateway. */
					*pulIPAddress = ulAddressToLookup;
				}
			}
		}
	}

	return eReturn;
}

/*-----------------------------------------------------------*/

static eARPLookupResult_t prvCacheLookup( uint32_t ulAddressToLookup, xMACAddress_t * const pxMACAddress )
{
BaseType_t x;
eARPLookupResult_t eReturn = eARPCacheMiss;

	/* Loop through each entry in the ARP cache. */
	for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
	{
		/* Does this row in the ARP cache table hold an entry for the IP address
		being queried? */
		if( xARPCache[ x ].ulIPAddress == ulAddressToLookup )
		{
			/* A matching valid entry was found. */
			if( xARPCache[ x ].ucValid == pdFALSE )
			{
				/* This entry is waiting an ARP reply, so is not valid. */
				eReturn = eCantSendPacket;
			}
			else
			{
				/* A valid entry was found. */
				memcpy( pxMACAddress->ucBytes, xARPCache[ x ].xMACAddress.ucBytes, sizeof( xMACAddress_t ) );
				eReturn = eARPCacheHit;
			}
			break;
		}
	}
	return eReturn;
}
/*-----------------------------------------------------------*/

void vARPAgeCache( void )
{
BaseType_t x;
TickType_t xTimeNow;

	/* Loop through each entry in the ARP cache. */
	for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
	{
		/* If the entry is valid (its age is greater than zero). */
		if( xARPCache[ x ].ucAge > 0U )
		{
			/* Decrement the age value of the entry in this ARP cache table row.
			When the age reaches zero it is no longer considered valid. */
			( xARPCache[ x ].ucAge )--;

			/* If the entry is not yet valid, then it is waiting an ARP
			reply, and the ARP request should be retransmitted. */
			if( xARPCache[ x ].ucValid == pdFALSE )
			{
				FreeRTOS_OutputARPRequest( xARPCache[ x ].ulIPAddress );
			}
			else if( xARPCache[ x ].ucAge <= arpMAX_ARP_AGE_BEFORE_NEW_ARP_REQUEST )
			{
				/* This entry will get removed soon.  See if the MAC address is
				still valid to prevent this happening. */
				iptraceARP_TABLE_ENTRY_WILL_EXPIRE( xARPCache[ x ].ulIPAddress );
				FreeRTOS_OutputARPRequest( xARPCache[ x ].ulIPAddress );
			}
			else
			{
				/* The age has just ticked down, with nothing to do. */
			}

			if( xARPCache[ x ].ucAge == 0 )
			{
				/* The entry is no longer valid.  Wipe it out. */
				iptraceARP_TABLE_ENTRY_EXPIRED( xARPCache[ x ].ulIPAddress );
				xARPCache[ x ].ulIPAddress = 0UL;
			}
		}
	}

	xTimeNow = xTaskGetTickCount ();

	if( ( ( xLastGratuitousARPTime == 0 ) || ( xTimeNow - xLastGratuitousARPTime ) > arpGRATUITOUS_ARP_PERIOD ) )
	{
		FreeRTOS_OutputARPRequest( *ipLOCAL_IP_ADDRESS_POINTER );
		xLastGratuitousARPTime = xTimeNow;
	}
}
/*-----------------------------------------------------------*/

void vARPSendGratuitous( void )
{
	/* Setting xLastGratuitousARPTime to 0 will force a gratuitous ARP the next
	time vARPAgeCache() is called. */
	xLastGratuitousARPTime = 0;

	/* Let the IP-task call vARPAgeCache(). */
	xSendEventToIPTask( eARPTimerEvent );
}

/*-----------------------------------------------------------*/
void FreeRTOS_OutputARPRequest( uint32_t ulIPAddress )
{
xNetworkBufferDescriptor_t *pxNetworkBuffer;

	/* This is called from the context of the IP event task, so a block time
	must not be used. */
	pxNetworkBuffer = pxGetNetworkBufferWithDescriptor( sizeof( xARPPacket_t ), 0 );

	if( pxNetworkBuffer != NULL )
	{
		pxNetworkBuffer->ulIPAddress = ulIPAddress;
		vARPGenerateRequestPacket( pxNetworkBuffer );
		xNetworkInterfaceOutput( pxNetworkBuffer, pdTRUE );
	}
}

void vARPGenerateRequestPacket( xNetworkBufferDescriptor_t * const pxNetworkBuffer )
{
xARPPacket_t *pxARPPacket;

	pxARPPacket = ( xARPPacket_t * ) pxNetworkBuffer->pucEthernetBuffer;

	/* memcpy the const part of the header information into the correct
	location in the packet.  This copies:
		xEthernetHeader.ulDestinationAddress
		xEthernetHeader.usFrameType;
		xARPHeader.usHardwareType;
		xARPHeader.usProtocolType;
		xARPHeader.ucHardwareAddressLength;
		xARPHeader.ucProtocolAddressLength;
		xARPHeader.usOperation;
		xARPHeader.xTargetHardwareAddress;
	*/
	memcpy( ( void * ) &( pxARPPacket->xEthernetHeader ), ( void * ) xDefaultPartARPPacketHeader, sizeof( xDefaultPartARPPacketHeader ) );
	memcpy( ( void * ) pxARPPacket->xEthernetHeader.xSourceAddress.ucBytes , ( void * ) ipLOCAL_MAC_ADDRESS, ( size_t ) ipMAC_ADDRESS_LENGTH_BYTES );
	memcpy( ( void * ) pxARPPacket->xARPHeader.xSenderHardwareAddress.ucBytes, ( void * ) ipLOCAL_MAC_ADDRESS, ( size_t ) ipMAC_ADDRESS_LENGTH_BYTES );
	/* HT:endian: network to network */
	pxARPPacket->xARPHeader.ulSenderProtocolAddress = *ipLOCAL_IP_ADDRESS_POINTER;
	pxARPPacket->xARPHeader.ulTargetProtocolAddress = pxNetworkBuffer->ulIPAddress;

	pxNetworkBuffer->xDataLength = sizeof( xARPPacket_t );

	iptraceCREATING_ARP_REQUEST( pxNetworkBuffer->ulIPAddress );
}
/*-----------------------------------------------------------*/

void FreeRTOS_ClearARP( void )
{
	memset( xARPCache, '\0', sizeof( xARPCache ) );
}
/*-----------------------------------------------------------*/

#if( ipconfigHAS_PRINTF != 0 ) || ( ipconfigHAS_DEBUG_PRINTF != 0 )

	void FreeRTOS_PrintARPCache( void )
	{
	BaseType_t x, xCount = 0;

		/* Loop through each entry in the ARP cache. */
		for( x = 0; x < ipconfigARP_CACHE_ENTRIES; x++ )
		{
			if( xARPCache[ x ].ulIPAddress != 0ul &&  xARPCache[ x ].ucAge > 0U )
			{
				/* See if the MAC-address also matches, and we're all happy */
				FreeRTOS_printf( ( "Arp %2ld: %3u - %16lxip : %02x:%02x:%02x : %02x:%02x:%02x\n",
					x,
					xARPCache[ x ].ucAge,
					xARPCache[ x ].ulIPAddress,
					xARPCache[ x ].xMACAddress.ucBytes[0],
					xARPCache[ x ].xMACAddress.ucBytes[1],
					xARPCache[ x ].xMACAddress.ucBytes[2],
					xARPCache[ x ].xMACAddress.ucBytes[3],
					xARPCache[ x ].xMACAddress.ucBytes[4],
					xARPCache[ x ].xMACAddress.ucBytes[5] ) );
				xCount++;
			}
		}

		FreeRTOS_printf( ( "Arp has %ld entries\n", xCount ) );
	}

#endif /* ( ipconfigHAS_PRINTF != 0 ) || ( ipconfigHAS_DEBUG_PRINTF != 0 ) */
