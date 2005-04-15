/*
 *  IOFireWireLibDevice.cpp
 *  IOFireWireFamily
 *
 *  Created by Niels on Thu Feb 27 2003.
 *  Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 *	$ Log:IOFireWireLibDevice.cpp,v $
 */

#import "IOFireWireLibDevice.h"
#import "IOFireWireLibCommand.h"
#import "IOFireWireLibUnitDirectory.h"
#import "IOFireWireLibConfigDirectory.h"
#import "IOFireWireLibPhysicalAddressSpace.h"
#import "IOFireWireLibPseudoAddressSpace.h"
#import "IOFireWireLibIsochChannel.h"
#import "IOFireWireLibIsochPort.h"
#import "IOFireWireLibDCLCommandPool.h"
#import "IOFireWireLibNuDCLPool.h"
#import "IOFireWireLibBufferFillIsochPort.h"

#import <IOKit/iokitmig.h>
#import <mach/mach.h>

namespace IOFireWireLib {

	Device::Device( const IUnknownVTbl & interface, CFDictionaryRef /*propertyTable*/, io_service_t service )
	: IOFireWireIUnknown( interface )
	{	
		if ( !service )
			throw kIOReturnBadArgument ;

		// mr. safety says, "initialize for safety!"
		mConnection					= nil ;
		mIsInited					= false ;
		mIsOpen						= false ;
		mNotifyIsOn					= false ;
		mAsyncPort					= 0 ;
		mAsyncCFPort				= 0 ;
		mBusResetAsyncRef[0] 		= 0 ;
		mBusResetDoneAsyncRef[0]	= 0 ;
		mBusResetHandler 			= 0 ;
		mBusResetDoneHandler 		= 0 ;
	
		mRunLoop					= 0 ;
		mRunLoopSource				= 0 ;
		mRunLoopMode				= 0 ;
	
		mIsochRunLoop				= 0 ;
		mIsochRunLoopSource			= 0 ;
		mIsochRunLoopMode			= 0 ;
	
		//
		// isoch related
		//
		mIsochAsyncPort				= 0 ;
		mIsochAsyncCFPort			= 0 ;

		mDefaultDevice = service ;

		IOReturn error = OpenDefaultConnection() ;
		if ( error )
			throw error ;
		
		// factory counting
		::CFPlugInAddInstanceForFactory( kIOFireWireLibFactoryID );

		mIsInited = true ;
	}

	Device::~Device()
	{
		if (mIsOpen)
			Close() ;
	
		if (mRunLoopSource)
		{
			RemoveDispatcherFromRunLoop( mRunLoop, mRunLoopSource, mRunLoopMode ) ;
			
			CFRelease( mRunLoopSource ) ;
			CFRelease( mRunLoop ) ;
			CFRelease( mRunLoopMode ) ;		
		}
		
		if ( mIsochRunLoopSource )
		{
			RemoveDispatcherFromRunLoop( mIsochRunLoop, mIsochRunLoopSource, mIsochRunLoopMode ) ;
	
			CFRelease( mIsochRunLoopSource ) ;
			CFRelease( mIsochRunLoop ) ;
			CFRelease( mIsochRunLoopMode ) ;		
		}
	
		// club ports to death
		if ( mIsochAsyncCFPort )
		{
			CFMachPortInvalidate( mIsochAsyncCFPort ) ;
			CFRelease( mIsochAsyncCFPort ) ;
			mach_port_destroy( mach_task_self(), mIsochAsyncPort ) ;
		}
		
		// club ports to death
		if ( mAsyncCFPort )
		{
			CFMachPortInvalidate( mAsyncCFPort ) ;
			CFRelease( mAsyncCFPort ) ;
			mach_port_destroy( mach_task_self(), mAsyncPort ) ;
		}
	
		if ( mConnection )
		{
			IOServiceClose( mConnection ) ;
		}
		
		if (mIsInited)
		{
			// factory counting
			::CFPlugInRemoveInstanceForFactory( kIOFireWireLibFactoryID );
		}
	}
	
	HRESULT STDMETHODCALLTYPE
	Device::QueryInterface(REFIID iid, LPVOID* ppv)
	{
		HRESULT		result = S_OK ;
		*ppv = nil ;
	
		CFUUIDRef	interfaceID	= CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, iid) ;
	
		if (	CFEqual(interfaceID, kIOFireWireDeviceInterfaceID )
	
				// v2 interfaces...
				||	CFEqual(interfaceID, kIOFireWireDeviceInterfaceID_v2)
				||	CFEqual(interfaceID, kIOFireWireNubInterfaceID)
				||	CFEqual(interfaceID, kIOFireWireUnitInterfaceID)
			
				// v3 interfaces...
				||	CFEqual( interfaceID, kIOFireWireNubInterfaceID_v3 )
				||	CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v3 )
				||	CFEqual( interfaceID, kIOFireWireUnitInterfaceID_v3 )

				// v4 interfaces...
				||	CFEqual( interfaceID, kIOFireWireNubInterfaceID_v4 )
				||	CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v4 )
				||	CFEqual( interfaceID, kIOFireWireUnitInterfaceID_v4 )

				// v5 interfaces...
				|| CFEqual( interfaceID, kIOFireWireNubInterfaceID_v5 )
				|| CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v5 )
				|| CFEqual( interfaceID, kIOFireWireUnitInterfaceID_v5 )
				
				// v6
				
				|| CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v6 )

				// v7
				
				|| CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v7 )
				
				// v8
				
				|| CFEqual( interfaceID, kIOFireWireDeviceInterfaceID_v8 )
			)
		{
			*ppv = & GetInterface() ;
			AddRef() ;
		}
		else
		{
			*ppv = nil ;
			result = E_NOINTERFACE ;
		}	
		
		CFRelease(interfaceID) ;
		
		return result ;
	}

#pragma mark -
	IOReturn
	Device::OpenDefaultConnection()
	{
		io_connect_t	connection	= 0 ;
		IOReturn		kr			= kIOReturnSuccess ;
		
		if ( 0 == mDefaultDevice )
			kr = kIOReturnNoDevice ;
		
		if (kIOReturnSuccess == kr )
			kr = IOServiceOpen(mDefaultDevice, mach_task_self(), kIOFireWireLibConnection, & connection) ;
	
		if (kIOReturnSuccess == kr )
			mConnection = connection ;
		
		return kr ;
	}
	
	IOReturn
	Device::CreateAsyncPorts()
	{
		IOReturn result = kIOReturnSuccess ;
	
		if (! mAsyncPort)
		{
			IOCreateReceivePort(kOSAsyncCompleteMessageID, & mAsyncPort) ;
			
			Boolean shouldFreeInfo ;
			CFMachPortContext cfPortContext	= {1, this, NULL, NULL, NULL} ;
			mAsyncCFPort = CFMachPortCreateWithPort(
								kCFAllocatorDefault,
								mAsyncPort,
								(CFMachPortCallBack) IODispatchCalloutFromMessage,
								& cfPortContext,
								& shouldFreeInfo) ;
			
			if (!mAsyncCFPort)
				result = kIOReturnNoMemory ;
		}
		
		return result ;
	}
	
	IOReturn
	Device::CreateIsochAsyncPorts()
	{
		IOReturn result = kIOReturnSuccess ;
	
		if (! mIsochAsyncPort)
		{
			IOCreateReceivePort(kOSAsyncCompleteMessageID, & mIsochAsyncPort) ;
			
			Boolean shouldFreeInfo ;
			CFMachPortContext cfPortContext	= {1, this, NULL, NULL, NULL} ;
			mIsochAsyncCFPort = CFMachPortCreateWithPort( kCFAllocatorDefault,
														mIsochAsyncPort,
														(CFMachPortCallBack) IODispatchCalloutFromMessage,
														& cfPortContext,
														& shouldFreeInfo) ;
			if (!mIsochAsyncCFPort)
				result = kIOReturnNoMemory ;		
		}
		
		return result ;
	}
	
	IOReturn
	Device::Open()
	{
		if ( mIsOpen )
		{
			return kIOReturnSuccess ;
		}
			
		IOReturn result = ::IOConnectMethodScalarIScalarO( GetUserClientConnection(), kOpen, 0, 0 ) ;
			
		mIsOpen = (kIOReturnSuccess == result) ;
	
		return result ;
	}
	
	IOReturn
	Device::OpenWithSessionRef(IOFireWireSessionRef session)
	{
		if (mIsOpen)
		{
			return kIOReturnSuccess ;
		}

		IOReturn result = ::IOConnectMethodScalarIScalarO( GetUserClientConnection(), kOpenWithSessionRef, 1, 0, session ) ;
			
		mIsOpen = (kIOReturnSuccess == result) ;
		
		return result ;
	}
	
	IOReturn
	Device::Seize( IOOptionBits flags )	// v3
	{
		if ( mIsOpen )
		{
			DebugLog("Device::Seize: Can't call while interface is open\n") ;
			return kIOReturnError ;
		}
		
		if ( nil == mConnection )
			return kIOReturnNoDevice ;
	
		IOReturn err = ::IOConnectMethodScalarIScalarO( GetUserClientConnection(), kSeize, 1, 0, flags ) ;
		DebugLogCond(err, "Could not seize service! err=%x\n", err) ;
		
		return err ;
	}
	
	void
	Device::Close()
	{	
		if (!mIsOpen)
		{
			DebugLog("Device::Close: interface not open\n") ;
			return ;
		}
	
		IOReturn err = kIOReturnSuccess;
		err = IOConnectMethodScalarIScalarO( GetUserClientConnection(), kClose, 0, 0 ) ;
			
		DebugLogCond( err, "Device::Close(): error %x returned from Close()!\n", err ) ;
	
		mIsOpen = false ;
	}
	
	#pragma mark -
	IOReturn
	Device::AddCallbackDispatcherToRunLoopForMode(	// v3+
		CFRunLoopRef 				runLoop,
		CFStringRef					runLoopMode )
	{
		// if the client passes 0 as the runloop, that means
		// we should remove the source instead of adding it.
	
		if ( !runLoop || !runLoopMode )
		{
			DebugLog("IOFireWireLibDeviceInterfaceImp::AddCallbackDispatcherToRunLoopForMode: runLoop == 0 || runLoopMode == 0\n") ;
			return kIOReturnBadArgument ;
		}
	
		IOReturn result = kIOReturnSuccess ;
		
		if (!AsyncPortsExist())
			result = CreateAsyncPorts() ;
	
		if ( kIOReturnSuccess == result )
		{
			CFRetain( runLoop ) ;
			CFRetain( runLoopMode ) ;
			
			mRunLoop 		= runLoop ;
			mRunLoopSource	= CFMachPortCreateRunLoopSource(
									kCFAllocatorDefault,
									GetAsyncCFPort(),
									0) ;
			mRunLoopMode	= runLoopMode ;
	
			if (!mRunLoopSource)
			{
				CFRelease( mRunLoop ) ;
				mRunLoop = 0 ;
				
				CFRelease( mRunLoopMode ) ;
				mRunLoopMode = 0 ;
				
				result = kIOReturnNoMemory ;
			}
			
			if ( kIOReturnSuccess == result )
				CFRunLoopAddSource(mRunLoop, mRunLoopSource, mRunLoopMode ) ;
		}
		
		return result ;
	}
	
	void
	Device::RemoveDispatcherFromRunLoop(
		CFRunLoopRef			runLoop,
		CFRunLoopSourceRef		runLoopSource,
		CFStringRef				mode)
	{
		if ( runLoop && runLoopSource )
			if (CFRunLoopContainsSource( runLoop, runLoopSource, mode ))
				CFRunLoopRemoveSource( runLoop, runLoopSource, mode );
	}
	
	const Boolean
	Device::TurnOnNotification(
		void*					callBackRefCon)
	{
		IOReturn				result					= kIOReturnSuccess ;
		io_scalar_inband_t		params ;
		mach_msg_type_number_t	size = 0 ;
		
		if ( nil == mConnection )
			result = kIOReturnNoDevice ;
		
		if (!AsyncPortsExist())
			result = kIOReturnError ;	// zzz  need a new error type meaning "you forgot to call AddDispatcherToRunLoop"
	
		if ( kIOReturnSuccess == result )
		{
			params[0]	= (UInt32)(IOAsyncCallback) & Device::BusResetHandler ;
			params[1]	= (UInt32) callBackRefCon; //(UInt32) this ;
		
			result = ::io_async_method_scalarI_scalarO( mConnection, mAsyncPort, mBusResetAsyncRef, 1, 
					kSetAsyncRef_BusReset, params, 2, params, & size ) ;
			
		}
	
		if ( kIOReturnSuccess == result )
		{
			params[0]	= (UInt32)(IOAsyncCallback) & Device::BusResetDoneHandler ;
			params[1]	= (UInt32) callBackRefCon; //(UInt32) this ;
			size = 0 ;
		
			result = ::io_async_method_scalarI_scalarO( mConnection, mAsyncPort, mBusResetDoneAsyncRef, 1, 
					kSetAsyncRef_BusResetDone, params, 2, params, & size) ;
			
		}
		
		if ( kIOReturnSuccess == result )
			mNotifyIsOn = true ;
			
		return ( kIOReturnSuccess == result ) ;
	}
	
	void
	Device::TurnOffNotification()
	{
		IOReturn				result			= kIOReturnSuccess ;
		io_scalar_inband_t		params ;
		mach_msg_type_number_t	size 		= 0 ;
		
		// if notification isn't on, skip out.
		if (!mNotifyIsOn)
			return ;
	
		if (!mConnection)
			result = kIOReturnNoDevice ;
		
		if ( kIOReturnSuccess == result )
		{
			params[0]	= (UInt32)(IOAsyncCallback) 0 ;
			params[1]	= 0 ;
		
			result = ::io_async_method_scalarI_scalarO( mConnection, mAsyncPort, mBusResetAsyncRef, 1, 
					kSetAsyncRef_BusReset, params, 2, params, & size) ;
			
			params[0]	= (UInt32)(IOAsyncCallback) 0 ;
			params[1]	= 0 ;
			size = 0 ;
		
			result = ::io_async_method_scalarI_scalarO( mConnection, mAsyncPort, mBusResetDoneAsyncRef, 1,
					kSetAsyncRef_BusResetDone, params, 2, params, & size) ;
			
		}
		
		mNotifyIsOn = false ;
	}
	
	
	const IOFireWireBusResetHandler
	Device::SetBusResetHandler(
		IOFireWireBusResetHandler			inBusResetHandler)
	{
		IOFireWireBusResetHandler	result = mBusResetHandler ;
		mBusResetHandler = inBusResetHandler ;
		
		return result ;
	}
	
	const IOFireWireBusResetDoneHandler
	Device::SetBusResetDoneHandler(
		IOFireWireBusResetDoneHandler		inBusResetDoneHandler)
	{
		IOFireWireBusResetDoneHandler	result = mBusResetDoneHandler ;
		mBusResetDoneHandler = inBusResetDoneHandler ;
		
		return result ;
	}
	
	void
	Device::BusResetHandler(
		void*							refCon,
		IOReturn						result)
	{
		Device*	me = IOFireWireIUnknown::InterfaceMap<Device>::GetThis(refCon) ;
	
		if (me->mBusResetHandler)
			(me->mBusResetHandler)( (IOFireWireLibDeviceRef)refCon, (FWClientCommandID) me) ;
	}
	
	void
	Device::BusResetDoneHandler(
		void*							refCon,
		IOReturn						result)
	{
		Device*	me = IOFireWireIUnknown::InterfaceMap<Device>::GetThis(refCon) ;
	
		if (me->mBusResetDoneHandler)
			(me->mBusResetDoneHandler)( (IOFireWireLibDeviceRef)refCon, (FWClientCommandID) me) ;
	}
	
#pragma mark -
	IOReturn
	Device::Read(
		io_object_t				device,
		const FWAddress &		addr,
		void*					buf,
		UInt32*					size,
		Boolean					failOnReset,
		UInt32					generation)
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		IOByteCount 			resultSize	 	= sizeof( *size ) ;
		ReadParams	 			params 			= { addr, buf, *size, failOnReset, generation, device == 0 /*isAbs*/ } ;
	
		return IOConnectMethodStructureIStructureO( mConnection, kRead, sizeof(params), & resultSize, & params, size ) ;
	}
	
	IOReturn
	Device::ReadQuadlet( io_object_t device, const FWAddress & addr, UInt32* val, Boolean failOnReset, 
		UInt32 generation )
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		IOByteCount 				resultSize 		= sizeof( *val ) ;
		ReadQuadParams	 			params 			= { addr, val, 1, failOnReset, generation, device == 0 /*isAbs*/ } ;
	
		return IOConnectMethodStructureIStructureO( mConnection, kReadQuad, sizeof(params), & resultSize, & params, val ) ;
	}
	
	IOReturn
	Device::Write(
		io_object_t				device,
		const FWAddress &		addr,
		const void*				buf,
		UInt32* 				size,
		Boolean					failOnReset,
		UInt32					generation)
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		IOByteCount 				resultSize 		= sizeof( *size ) ;
		WriteParams		 			params 			= { addr, buf, *size, failOnReset, generation, device == 0 /*isAbs*/ } ;
	
		return IOConnectMethodStructureIStructureO( mConnection, kWrite, sizeof(params), & resultSize, & params, size ) ;
	}
	
	IOReturn
	Device::WriteQuadlet(
		io_object_t				device,
		const FWAddress &		addr,
		const UInt32			val,
		Boolean 				failOnReset,
		UInt32					generation)
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		WriteQuadParams 			params 			= { addr, val, failOnReset, generation, device == 0 } ;
		IOByteCount 				resultSize 		= 0 ;
	
		return IOConnectMethodStructureIStructureO( mConnection, kWriteQuad, sizeof(params), & resultSize, & params, nil ) ;
	}
	
	IOReturn
	Device::CompareSwap(
		io_object_t				device,
		const FWAddress &		addr,
		UInt32 					cmpVal,
		UInt32 					newVal,
		Boolean 				failOnReset,
		UInt32					generation)
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		IOByteCount 				resultSize 		= sizeof(UInt64) ;
		UInt64						result ;
		CompareSwapParams			params ;
		
		params.addr					= addr ;
		*(UInt32*)&params.cmpVal	= cmpVal ;
		*(UInt32*)&params.swapVal	= newVal ;
		params.size					= 1 ;
		params.failOnReset			= failOnReset ;
		params.generation			= generation ;
		params.isAbs				= device == 0 ;
	
		return IOConnectMethodStructureIStructureO( mConnection, kCompareSwap, sizeof(params), & resultSize, & params, & result ) ;
	}
	
	IOReturn
	Device::CompareSwap64(
		io_object_t				device,
		const FWAddress &		addr,
		UInt32*					expectedVal,
		UInt32*					newVal,
		UInt32*					oldVal,
		IOByteCount				size,
		Boolean 				failOnReset,
		UInt32					generation)
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
		if ( device != mDefaultDevice && device != 0 )
			return kIOReturnBadArgument ;
	
		CompareSwapParams			params ;
		
		// config params
		params.addr				= addr ;
		if ( size==4 )
		{
			*(UInt32*)&params.cmpVal	= expectedVal[0] ;
			*(UInt32*)&params.swapVal	= newVal[0] ;
		}
		else
		{
			params.cmpVal			= *(UInt64*)expectedVal ;
			params.swapVal			= *(UInt64*)newVal ;
		}
	
		params.size				= size >> 2 ;
		params.failOnReset		= failOnReset ;
		params.generation		= generation ;
		params.isAbs			= device == 0 ;
	
		UInt64			result ;
		IOByteCount 	resultSize 	= sizeof(UInt64) ;
		IOReturn error = ::IOConnectMethodStructureIStructureO( mConnection, kCompareSwap, sizeof(params), 
				& resultSize, & params, & result ) ;
		
		if (size==4)
		{
			oldVal[0] = *(UInt32*)&result ;
			if ( oldVal[0] != expectedVal[0] )
				error = kIOReturnCannotLock ;
		}
		else
		{
			*(UInt64*)oldVal = result ;
			if ( *(UInt64*)expectedVal != result )
				error = kIOReturnCannotLock ;
		}
		
		return error ;
	}
	
	#pragma mark -
	#pragma mark --- command objects ----------
	
	IOFireWireLibCommandRef	
	Device::CreateReadCommand(
		io_object_t 		device, 
		const FWAddress&	addr, 
		void* 				buf, 
		UInt32 				size, 
		IOFireWireLibCommandCallback callback,
		Boolean 			failOnReset, 
		UInt32 				generation,
		void*				inRefCon,
		REFIID				iid)
	{
		IOFireWireLibCommandRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = ReadCmd::Alloc(*this, device, addr, buf, size, callback, failOnReset, generation, inRefCon) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
	
		return result ;
	}
	
	IOFireWireLibCommandRef	
	Device::CreateReadQuadletCommand(
		io_object_t 		device, 
		const FWAddress & 	addr, 
		UInt32	 			quads[], 
		UInt32				numQuads,
		IOFireWireLibCommandCallback callback,
		Boolean 			failOnReset, 
		UInt32				generation,
		void*				inRefCon,
		REFIID				iid)
	{
		IOFireWireLibCommandRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = ReadQuadCmd::Alloc(*this, device, addr, quads, numQuads, callback, failOnReset, generation, inRefCon) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	IOFireWireLibCommandRef
	Device::CreateWriteCommand(
		io_object_t 		device, 
		const FWAddress & 	addr, 
		void*		 		buf, 
		UInt32 				size, 
		IOFireWireLibCommandCallback callback,
		Boolean 			failOnReset, 
		UInt32 				generation,
		void*				inRefCon,
		REFIID				iid)
	{
		IOFireWireLibCommandRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = WriteCmd::Alloc(*this, device, addr, buf, size, callback, failOnReset, generation, inRefCon) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
		
	}
	
	IOFireWireLibCommandRef
	Device::CreateWriteQuadletCommand(
		io_object_t 		device, 
		const FWAddress & 	addr, 
		UInt32		 		quads[], 
		UInt32				numQuads,
		IOFireWireLibCommandCallback callback,
		Boolean 			failOnReset, 
		UInt32 				generation,
		void*				inRefCon,
		REFIID				iid)
	{
		IOFireWireLibCommandRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = WriteQuadCmd::Alloc(*this, device, addr, quads, numQuads, callback, failOnReset, generation, inRefCon) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	IOFireWireLibCommandRef
	Device::CreateCompareSwapCommand( io_object_t device, const FWAddress & addr, UInt64 cmpVal, UInt64 newVal, 
		unsigned int quads, IOFireWireLibCommandCallback callback, Boolean failOnReset, UInt32 generation,
		void* inRefCon, REFIID iid)
	{
		IOFireWireLibCommandRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = CompareSwapCmd::Alloc( *this, device, addr, cmpVal, newVal, quads, callback, failOnReset, generation, inRefCon) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	#pragma mark -
	IOReturn
	Device::BusReset()
	{
		if (!mIsOpen)
			return kIOReturnNotOpen ;
	
		return IOConnectMethodScalarIScalarO( mConnection, kBusReset, 0, 0 ) ;
	}
	
	IOReturn
	Device::GetCycleTime(
		UInt32*		outCycleTime)
	{
		return IOConnectMethodScalarIScalarO( mConnection, kCycleTime, 0, 1, outCycleTime ) ;
	}
	
	IOReturn
	Device::GetBusCycleTime(
		UInt32*		outBusTime,
		UInt32*		outCycleTime)
	{
		return IOConnectMethodScalarIScalarO( mConnection, kGetBusCycleTime, 0, 2, outBusTime, outCycleTime ) ;
	}
	
	IOReturn
	Device::GetGenerationAndNodeID(
		UInt32*		outGeneration,
		UInt16*		outNodeID)
	{
		UInt32	nodeID ;
		IOReturn result = IOConnectMethodScalarIScalarO( mConnection, kGetGenerationAndNodeID, 0, 2, outGeneration, & nodeID ) ;
		
		*outNodeID = (UInt16)nodeID ;
	
		return result ;
	}
	
	IOReturn
	Device::GetLocalNodeID(
		UInt16*		outLocalNodeID)
	{
		UInt32	localNodeID ;
	
		IOReturn result = IOConnectMethodScalarIScalarO( mConnection, kGetLocalNodeID, 0, 1, & localNodeID ) ;
		*outLocalNodeID = (UInt16)localNodeID ;
		
		return result ;
	}
	
	IOReturn
	Device::GetResetTime(
		AbsoluteTime*			resetTime)
	{
		IOByteCount		size = sizeof(*resetTime) ;
		return IOConnectMethodStructureIStructureO( mConnection, kGetResetTime, 0, & size, NULL, resetTime ) ;
	}
	
	#pragma mark -
	IOFireWireLibLocalUnitDirectoryRef
	Device::CreateLocalUnitDirectory( REFIID iid )
	{
		IOFireWireLibLocalUnitDirectoryRef	result = 0 ;
	
		if (mIsOpen)
		{
				// we allocate a user space pseudo address space with the reference we
				// got from the kernel
			IUnknownVTbl**	iUnknown = reinterpret_cast<IUnknownVTbl**>(LocalUnitDirectory::Alloc(*this)) ;
						
				// we got a new iUnknown from the object. Query it for the interface
				// requested in iid...
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
						
				// we got the interface we wanted (or at least another ref to iUnknown),
				// so we call iUnknown.Release()
			(*iUnknown)->Release(iUnknown) ;
		}
			
		return result ;
	}
	
	IOFireWireLibConfigDirectoryRef
	Device::GetConfigDirectory(
		REFIID				iid)
	{
		IOFireWireLibConfigDirectoryRef	result = 0 ;
		
		// we allocate a user space config directory space with the reference we
		// got from the kernel
		IUnknownVTbl**	iUnknown	= reinterpret_cast<IUnknownVTbl**>(ConfigDirectoryCOM::Alloc(*this)) ;

		// we got a new iUnknown from the object. Query it for the interface
		// requested in iid...
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
		
			// we got the interface we wanted (or at least another ref to iUnknown),
			// so we call iUnknown.Release()
			(*iUnknown)->Release(iUnknown) ;
		}
	
		return result ;
	}
	
	IOFireWireLibConfigDirectoryRef
	Device::CreateConfigDirectoryWithIOObject(
		io_object_t			inObject,
		REFIID				iid)
	{
		IOFireWireLibConfigDirectoryRef	result = 0 ;
		
		// we allocate a user space pseudo address space with the reference we
		// got from the kernel
		IUnknownVTbl**	iUnknown	= reinterpret_cast<IUnknownVTbl**>(ConfigDirectoryCOM::Alloc(*this, (UserObjectHandle)inObject)) ;
	
		// we got a new iUnknown from the object. Query it for the interface
		// requested in iid...
		(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			
		// we got the interface we wanted (or at least another ref to iUnknown),
		// so we call iUnknown.Release()
		(*iUnknown)->Release(iUnknown) ;
	
		return result ;
	}
	
	IOFireWireLibPhysicalAddressSpaceRef
	Device::CreatePhysicalAddressSpace(
		UInt32						inSize,
		void*						inBackingStore,
		UInt32						inFlags,
		REFIID						iid)	// flags unused
	{
		IOFireWireLibPhysicalAddressSpaceRef	result = 0 ;
		
		if ( mIsOpen )
		{
			UserObjectHandle	output ;
			
			if ( kIOReturnSuccess  == IOConnectMethodScalarIScalarO ( mConnection, kPhysicalAddrSpace_Allocate,
																			3, 1, inSize, inBackingStore, inFlags, & output ) )
			{
				if (output)
				{
					// we allocate a user space pseudo address space with the reference we
					// got from the kernel
					IUnknownVTbl**	iUnknown = PhysicalAddressSpace::Alloc(*this, output, inSize, inBackingStore, inFlags) ;
					
					// we got a new iUnknown from the object. Query it for the interface
					// requested in iid...
					if (iUnknown)
					{
						(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
						
						// we got the interface we wanted (or at least another ref to iUnknown),
						// so we call iUnknown.Release()
						(*iUnknown)->Release(iUnknown) ;
					}
				}
			}			
	
		}
		
		return result ;
	}
	
	IOFireWireLibPseudoAddressSpaceRef
	Device::CreateAddressSpace(
		UInt32						inSize,
		void*						inRefCon,
		UInt32						inQueueBufferSize,
		void*						inBackingStore,
		UInt32						inFlags,
		REFIID						iid,
		Boolean						isInitialUnits,
		UInt32						inAddressLo )
	{
		if (!mIsOpen)
		{
			DebugLog( "Device::CreatePseudoAddressSpace: no connection or device is not open\n") ;
			return 0 ;
		}
		
		if ( !inBackingStore && ( (inFlags & kFWAddressSpaceAutoWriteReply != 0) || (inFlags & kFWAddressSpaceAutoReadReply != 0) || (inFlags & kFWAddressSpaceAutoCopyOnWrite != 0) ) )
		{
			DebugLog( "Can't create address space with nil backing store!\n" ) ;
			return 0 ;
		}
		
		IOFireWireLibPseudoAddressSpaceRef				result = 0 ;
	
		void*	queueBuffer = nil ;
		if ( inQueueBufferSize > 0 )
			queueBuffer	= new Byte[inQueueBufferSize] ;
	
		AddressSpaceCreateParams	params ;
		params.size 			= inSize ;
		params.queueBuffer 		= (Byte*) queueBuffer ;
		params.queueSize		= (UInt32) inQueueBufferSize ;
		params.backingStore 	= (Byte*) inBackingStore ;
		params.refCon			= this ;
		params.flags			= inFlags ;
		params.isInitialUnits	= isInitialUnits ;
		params.addressLo		= inAddressLo ;
		
		UserObjectHandle	addrSpaceRef ;
		IOByteCount			size	= sizeof(addrSpaceRef) ;
		
		// call the routine which creates a pseudo address space in the kernel.
		IOReturn err = ::IOConnectMethodStructureIStructureO( mConnection, kPseudoAddrSpace_Allocate,
										sizeof(params), & size, & params, & addrSpaceRef ) ;
	
		if ( !err )
		{
			// we allocate a user space pseudo address space with the reference we
			// got from the kernel
			IUnknownVTbl**	iUnknown = PseudoAddressSpace::Alloc(*this, addrSpaceRef, queueBuffer, inQueueBufferSize, inBackingStore, inRefCon) ;
			
			// we got a new iUnknown from the object. Query it for the interface
			// requested in iid...
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			
			// we got the interface we wanted (or at least another ref to iUnknown),
			// so we call iUnknown.Release()
			(*iUnknown)->Release(iUnknown) ;
			
		}
		
		return result ;
	}
	
	
	#pragma mark -
	IOReturn
	Device::FireBugMsg(
		const char *	inMsg)
	{
		IOReturn result = kIOReturnSuccess ;
		UInt32 size = strlen(inMsg) + 1 ;
		
		if (!mIsOpen)
			result = kIOReturnNotOpen ;
	
		{
			UInt32		generation = 0 ;
			UInt16		nodeID ;
	
			if ( kIOReturnSuccess == result )
				result = GetGenerationAndNodeID( & generation, & nodeID ) ;
	
			if (kIOReturnSuccess == result)
			{
				result = Write(0, FWAddress(0x4242, 0x42424242, 0x4242), inMsg, & size, true, generation) ;
				
				// firebug never responds, so just change a timeout err into success
				if ( kIOReturnTimeout == result )
					result = kIOReturnSuccess ;
			}
		}
		
		return result ;
	}
	
	IOReturn
	Device::FireLog(
		const char *	format, 
		va_list			ap )
	{
		char		msg[128] ;
		vsnprintf( msg, 127, format, ap ) ;
	
		IOByteCount outSize = 0 ;
		
		IOReturn	err = ::IOConnectMethodStructureIStructureO( GetUserClientConnection(), kFireLog, strlen( msg )+1, & outSize, msg, nil ) ;
		return err ;
	}
	
	IOReturn
	Device::FireLog( const char* format, ... )
	{
		IOReturn error = kIOReturnSuccess;
		
		va_list		ap ;
		va_start( ap, format ) ;
		
		error = FireLog( format, ap ) ;
	
		va_end( ap ) ;	
		
		return error ;	
	}
	
	IOReturn
	Device::CreateCFStringWithOSStringRef(
		UserObjectHandle	inStringRef,
		UInt32				inStringLen,
		CFStringRef*&		text)
	{
		char*				textBuffer = new char[inStringLen+1] ;
		io_connect_t		connection = GetUserClientConnection() ;
		UInt32				stringSize ;
		
		if (!textBuffer)
			return kIOReturnNoMemory ;
		
		IOReturn result = ::IOConnectMethodScalarIScalarO( connection, kGetOSStringData, 3, 1, inStringRef, 
				inStringLen, textBuffer, & stringSize) ;
	
		textBuffer[inStringLen] = 0 ;
		
		if (text && (kIOReturnSuccess == result))
			*text = CFStringCreateWithCString(kCFAllocatorDefault, textBuffer, kCFStringEncodingASCII) ;
		
		delete textBuffer ;
	
		return result ;
	}
	
	IOReturn
	Device::CreateCFDataWithOSDataRef(
		UserObjectHandle		inDataRef,
		IOByteCount			inDataSize,
		CFDataRef*&			data)
	{
		UInt8*			buffer = new UInt8[inDataSize] ;
		IOByteCount		dataSize ;
		
		if (!buffer)
			return kIOReturnNoMemory ;
			
		if (!mConnection)
			return kIOReturnError ;
		
		IOReturn result = IOConnectMethodScalarIScalarO( mConnection, kGetOSDataData, 3, 1,
									inDataRef, inDataSize, buffer, & dataSize) ;
	
		if (data && (kIOReturnSuccess == result))
			*data = CFDataCreate(kCFAllocatorDefault, buffer, inDataSize) ;
	
		if (!data)
			result = kIOReturnNoMemory ;
			
		delete buffer ;
		
		return result ;
	}
	
	// 
	// --- isoch related
	//
	IOReturn
	Device::AddIsochCallbackDispatcherToRunLoopForMode( // v3+
		CFRunLoopRef			runLoop,
		CFStringRef 			runLoopMode )
	{
		IOReturn result = kIOReturnSuccess ;
		
		if ( !runLoop || !runLoopMode )
		{
			DebugLog("Device::AddIsochCallbackDispatcherToRunLoopForMode: runLoop==0 || runLoopMode==0\n") ;
			return kIOReturnBadArgument ;
		}
	
		if (!IsochAsyncPortsExist())
			result = CreateIsochAsyncPorts() ;
	
		if ( kIOReturnSuccess == result )
		{
			CFRetain(runLoop) ;
			CFRetain(runLoopMode) ;
			
			mIsochRunLoop 			= runLoop ;
			mIsochRunLoopSource		= CFMachPortCreateRunLoopSource( kCFAllocatorDefault,
																	GetIsochAsyncCFPort() ,
																	0) ;
			mIsochRunLoopMode		= runLoopMode ;
	
			if (!mIsochRunLoopSource)
			{
				CFRelease( mIsochRunLoop ) ;
				CFRelease( mIsochRunLoopMode ) ;
				result = kIOReturnNoMemory ;
			}
			
			if ( kIOReturnSuccess == result )
				::CFRunLoopAddSource( mIsochRunLoop, mIsochRunLoopSource, mIsochRunLoopMode ) ;
		}
		
		return result ;
	}
	
	void
	Device::RemoveIsochCallbackDispatcherFromRunLoop()
	{
		RemoveDispatcherFromRunLoop( mIsochRunLoop, mIsochRunLoopSource, mIsochRunLoopMode ) ;
		
		CFRelease(mIsochRunLoop) ;
		mIsochRunLoop = 0 ;
		
		CFRelease( mIsochRunLoopSource ) ;
		mIsochRunLoopSource = 0 ;
		
		CFRelease( mIsochRunLoopMode );
		mIsochRunLoopMode = 0 ;
	}
		
	IOFireWireLibRemoteIsochPortRef
	Device::CreateRemoteIsochPort(
		Boolean					inTalking,
		REFIID 					iid)
	{
		IOFireWireLibRemoteIsochPortRef		result = 0 ;
		
		IUnknownVTbl** iUnknown = RemoteIsochPortCOM::Alloc(*this, inTalking) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	IOFireWireLibLocalIsochPortRef
	Device::CreateLocalIsochPortWithOptions(
		Boolean					talking,
		DCLCommand*				dclProgram,
		UInt32					startEvent,
		UInt32					startState,
		UInt32					startMask,
		IOVirtualRange			dclProgramRanges[],			// optional optimization parameters
		UInt32					dclProgramRangeCount,
		IOVirtualRange			bufferRanges[],
		UInt32					bufferRangeCount,
		IOFWIsochPortOptions	options,
		REFIID 					iid)
	{
		IOFireWireLibLocalIsochPortRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = LocalIsochPortCOM::Alloc(*this, talking, dclProgram, startEvent, startState, 
															startMask, dclProgramRanges, dclProgramRangeCount, bufferRanges, bufferRangeCount, options ) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	IOReturn
	Device::GetBusGeneration( UInt32* outGeneration )
	{
		return IOConnectMethodScalarIScalarO( mConnection, kGetBusGeneration, 0, 1, outGeneration ) ;
	}
	
	IOReturn
	Device::GetLocalNodeIDWithGeneration( UInt32 checkGeneration, UInt16* outLocalNodeID )
	{
		UInt32	nodeID ;
		IOReturn error = IOConnectMethodScalarIScalarO( mConnection, kGetLocalNodeIDWithGeneration, 1, 1, checkGeneration, & nodeID ) ;
		
		*outLocalNodeID = (UInt16)nodeID ;
		
		return error ;
	}
	
	IOReturn
	Device::GetRemoteNodeID( UInt32 checkGeneration, UInt16* outRemoteNodeID )
	{
		UInt32	nodeID ;
		IOReturn error = IOConnectMethodScalarIScalarO( mConnection, kGetRemoteNodeID, 1, 1, checkGeneration, & nodeID ) ;
		
		*outRemoteNodeID = (UInt16)nodeID ;
		
		return error ;
	}
	
	IOReturn
	Device::GetSpeedToNode( UInt32 checkGeneration, IOFWSpeed* outSpeed)
	{
		UInt32	speed ;
		IOReturn error = IOConnectMethodScalarIScalarO( mConnection, kGetSpeedToNode, 1, 1, checkGeneration, & speed ) ;

		*outSpeed = (IOFWSpeed)speed ;
		
		return error ;
	}
	
	IOReturn
	Device::GetSpeedBetweenNodes( UInt32 checkGeneration, UInt16 srcNodeID, UInt16 destNodeID, IOFWSpeed* outSpeed)
	{
		UInt32 speed ;
		
		IOReturn error = IOConnectMethodScalarIScalarO( mConnection, kGetSpeedBetweenNodes, 3, 1, checkGeneration, (UInt32)srcNodeID, (UInt32)destNodeID, & speed ) ;
		
		*outSpeed = (IOFWSpeed)speed ;
		
		return error ;
	}

	IOReturn
	Device::GetIRMNodeID( UInt32 checkGeneration, UInt16* outIRMNodeID )
	{
		UInt32 tempNodeID ;
		
		IOReturn error = ::IOConnectMethodScalarIScalarO( mConnection, kGetIRMNodeID, 1, 1, checkGeneration, & tempNodeID ) ;

		*outIRMNodeID = (UInt16)tempNodeID ;
		
		return error ;
	}
	
	IOReturn
	Device::ClipMaxRec2K( Boolean clipMaxRec )
	{
		//fprintf(stderr, "Device::ClipMaxRec2K\n") ;
	
		IOReturn error = ::IOConnectMethodScalarIScalarO( mConnection, kClipMaxRec2K, 1, 0, clipMaxRec ) ;
		
		return error ;
	}

	IOFireWireSessionRef
	Device::GetSessionRef()
	{
		IOFireWireSessionRef sessionRef = 0 ;
		IOReturn error = ::IOConnectMethodScalarIScalarO( mConnection, kGetSessionRef, 0, 1, & sessionRef ) ;

		DebugLogCond( error, "Device::GetSessionRef error=%x\n", error ) ;

		return sessionRef;
	}
	
#pragma mark -
	const IOFireWireDeviceInterface DeviceCOM::sInterface = 
	{
		INTERFACEIMP_INTERFACE,
		8, 0, // version/revision
		
		& DeviceCOM::SInterfaceIsInited,
		& DeviceCOM::SGetDevice,
		& DeviceCOM::SOpen,
		& DeviceCOM::SOpenWithSessionRef,
		& DeviceCOM::SClose,
		& DeviceCOM::SNotificationIsOn,
		& DeviceCOM::SAddCallbackDispatcherToRunLoop,
		& DeviceCOM::SRemoveCallbackDispatcherFromRunLoop,
		& DeviceCOM::STurnOnNotification,
		& DeviceCOM::STurnOffNotification,
		& DeviceCOM::SSetBusResetHandler,
		& DeviceCOM::SSetBusResetDoneHandler,
		& DeviceCOM::SClientCommandIsComplete,
		
		// --- FireWire send/recv methods --------------
		& DeviceCOM::SRead,
		& DeviceCOM::SReadQuadlet,
		& DeviceCOM::SWrite,
		& DeviceCOM::SWriteQuadlet,
		& DeviceCOM::SCompareSwap,
		
		// --- firewire commands -----------------------
		& DeviceCOM::SCreateReadCommand,
		& DeviceCOM::SCreateReadQuadletCommand,
		& DeviceCOM::SCreateWriteCommand,
		& DeviceCOM::SCreateWriteQuadletCommand,
		& DeviceCOM::SCreateCompareSwapCommand,
	
			// --- other methods ---------------------------
		& DeviceCOM::SBusReset,
		& DeviceCOM::SGetCycleTime,
		& DeviceCOM::SGetGenerationAndNodeID,
		& DeviceCOM::SGetLocalNodeID,
		& DeviceCOM::SGetResetTime,
	
			// --- unit directory support ------------------
		& DeviceCOM::SCreateLocalUnitDirectory,
	
		& DeviceCOM::SGetConfigDirectory,
		& DeviceCOM::SCreateConfigDirectoryWithIOObject,
			// --- address space support -------------------
		& DeviceCOM::SCreatePseudoAddressSpace,
		& DeviceCOM::SCreatePhysicalAddressSpace,
			
			// --- debugging -------------------------------
		& DeviceCOM::SFireBugMsg,
		
			// --- isoch -----------------------------------
		& DeviceCOM::SAddIsochCallbackDispatcherToRunLoop,
		& DeviceCOM::SCreateRemoteIsochPort,
		& DeviceCOM::SCreateLocalIsochPort,
		& DeviceCOM::SCreateIsochChannel,
		& DeviceCOM::SCreateDCLCommandPool,
	
			// --- refcon ----------------------------------
		& DeviceCOM::SGetRefCon,
		& DeviceCOM::SSetRefCon,
	
			// --- debugging -------------------------------
		// do not use this function
	//	& DeviceCOM::SGetDebugProperty,
		NULL,
		& DeviceCOM::SPrintDCLProgram,
	
			// --- initial units address space -------------	
		& DeviceCOM::SCreateInitialUnitsPseudoAddressSpace,
	
			// --- callback dispatcher utils (cont.) -------
		& DeviceCOM::SAddCallbackDispatcherToRunLoopForMode,
		& DeviceCOM::SAddIsochCallbackDispatcherToRunLoopForMode,
		& DeviceCOM::SRemoveIsochCallbackDispatcherFromRunLoop,
		
			// --- seize service ---------------------------
		& DeviceCOM::SSeize,
		
			// --- more debugging --------------------------
		& DeviceCOM::SFireLog,
	
			// --- other methods --- new in v3
		& DeviceCOM::SGetBusCycleTime,
		
		//
		// v4
		//
		& DeviceCOM::SCreateCompareSwapCommand64,
		& DeviceCOM::SCompareSwap64,
		& DeviceCOM::SGetBusGeneration,
		& DeviceCOM::SGetLocalNodeIDWithGeneration,
		& DeviceCOM::SGetRemoteNodeID,
		& DeviceCOM::SGetSpeedToNode,
		& DeviceCOM::SGetSpeedBetweenNodes,
		
		//
		// v5/
		//
		
		& DeviceCOM::S_GetIRMNodeID

		//
		// v6
		//
		
		,& DeviceCOM::S_ClipMaxRec2K
		,& DeviceCOM::S_CreateNuDCLPool
		
		//
		// v7
		//
		
		, & DeviceCOM::S_GetSessionRef
		
		//
		// v8
		//
		
		, & DeviceCOM :: S_CreateLocalIsochPortWithOptions

	} ;
	
	DeviceCOM::DeviceCOM( CFDictionaryRef propertyTable, io_service_t service )
	: Device( reinterpret_cast<const IUnknownVTbl &>( sInterface ), propertyTable, service )
	{
	}
	
	// ============================================================
	// static allocator
	// ============================================================
	
	IOFireWireDeviceInterface** 
	DeviceCOM::Alloc( CFDictionaryRef propertyTable, io_service_t service )
	{
		DeviceCOM*	me = nil ;
		
		try {
			me = new DeviceCOM( propertyTable, service ) ;
		} catch ( ... ) {
		}

		if( !me )
			return nil ;

		return reinterpret_cast<IOFireWireDeviceInterface**>(&me->GetInterface()) ;
	}

	Boolean
	DeviceCOM::SInterfaceIsInited(IOFireWireLibDeviceRef self) 
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->mIsInited;
	}
	
	io_object_t
	DeviceCOM::SGetDevice(IOFireWireLibDeviceRef self)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->mDefaultDevice; 
	}
	
	IOReturn
	DeviceCOM::SOpen(IOFireWireLibDeviceRef self) 
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->Open() ;
	}
	
	IOReturn
	DeviceCOM::SOpenWithSessionRef(IOFireWireLibDeviceRef self, IOFireWireSessionRef session) 
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->OpenWithSessionRef(session) ;
	}
	
	IOReturn
	DeviceCOM::SSeize(		// v3+
		IOFireWireLibDeviceRef	 	self,
		IOOptionBits				flags,
		... )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->Seize( flags ) ;
	}
	
	void
	DeviceCOM::SClose(IOFireWireLibDeviceRef self)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->Close() ;
	}
		
	// --- FireWire notification methods --------------
	const Boolean
	DeviceCOM::SNotificationIsOn(IOFireWireLibDeviceRef self)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->mNotifyIsOn; 
	}
	
	const IOReturn
	DeviceCOM::SAddCallbackDispatcherToRunLoop(IOFireWireLibDeviceRef self, CFRunLoopRef runLoop)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->AddCallbackDispatcherToRunLoop(runLoop) ;
	}
	
	IOReturn
	DeviceCOM::SAddCallbackDispatcherToRunLoopForMode(	// v3+
		IOFireWireLibDeviceRef 		self, 
		CFRunLoopRef 				runLoop,
		CFStringRef					runLoopMode )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->AddCallbackDispatcherToRunLoopForMode( runLoop, runLoopMode ) ;
	}
	
	const void
	DeviceCOM::SRemoveCallbackDispatcherFromRunLoop(IOFireWireLibDeviceRef self)
	{
		DeviceCOM*	me = IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis( self ) ;
		me->RemoveDispatcherFromRunLoop( me->mRunLoop, me->mRunLoopSource, me->mRunLoopMode ) ;
		
		if ( me->mRunLoop )
		{
			CFRelease( me->mRunLoop ) ;
			me->mRunLoop = 0 ;
		}
		
		if ( me->mRunLoopSource )
		{
			CFRelease( me->mRunLoopSource ) ;
			me->mRunLoopSource = 0 ;
		}
		
		if ( me->mRunLoopMode )
		{
			CFRelease( me->mRunLoopMode) ;
			me->mRunLoopMode = 0 ;
		}
	}
	
	// Makes notification active. Returns false if notification could not be activated.
	const Boolean
	DeviceCOM::STurnOnNotification(IOFireWireLibDeviceRef self)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->TurnOnNotification( self ) ;
	}
	
	// Notification callbacks will no longer be called.
	void
	DeviceCOM::STurnOffNotification(IOFireWireLibDeviceRef self)
	{
		IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->TurnOffNotification() ;
	}
	
	const IOFireWireBusResetHandler
	DeviceCOM::SSetBusResetHandler(IOFireWireLibDeviceRef self, IOFireWireBusResetHandler inBusResetHandler)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->SetBusResetHandler(inBusResetHandler) ;
	}
	
	const IOFireWireBusResetDoneHandler
	DeviceCOM::SSetBusResetDoneHandler(IOFireWireLibDeviceRef self, IOFireWireBusResetDoneHandler inBusResetDoneHandler) 
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->SetBusResetDoneHandler(inBusResetDoneHandler) ;
	}
	
	void
	DeviceCOM::SClientCommandIsComplete(IOFireWireLibDeviceRef self, FWClientCommandID commandID, IOReturn status)
	{ 
		IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->ClientCommandIsComplete(commandID, status); 
	}
	
	IOReturn
	DeviceCOM::SRead(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress * addr, void* buf, 
		UInt32* size, Boolean failOnReset, UInt32 generation)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->Read(device, *addr, buf, size, failOnReset, generation) ; 
	}
	
	IOReturn
	DeviceCOM::SReadQuadlet(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress * addr,
		UInt32* val, Boolean failOnReset, UInt32 generation)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->ReadQuadlet(device, *addr, val, failOnReset, generation); 
	}
	
	IOReturn
	DeviceCOM::SWrite(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress * addr, const void* buf, UInt32* size,
		Boolean failOnReset, UInt32 generation)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->Write(device, *addr, buf, size, failOnReset, generation) ;
	}
	
	IOReturn
	DeviceCOM::SWriteQuadlet(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress* addr, const UInt32 val, 
		Boolean failOnReset, UInt32 generation)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->WriteQuadlet(device, *addr, val, failOnReset, generation) ;
	}
	
	IOReturn
	DeviceCOM::SCompareSwap(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress* addr, UInt32 cmpVal, UInt32 newVal, Boolean failOnReset, UInt32 generation)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CompareSwap(device, *addr, cmpVal, newVal, failOnReset, generation) ;
	}
	
	//
	// v4
	//
	IOReturn
	DeviceCOM::SCompareSwap64( IOFireWireLibDeviceRef self, io_object_t device, const FWAddress* addr, 
			UInt32* expectedVal, UInt32* newVal, UInt32* oldVal, IOByteCount size, Boolean failOnReset,
			UInt32 generation)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CompareSwap64( device, *addr, expectedVal, 
				newVal, oldVal, size, failOnReset, generation ) ;
	}

	//
	// v4
	//
	IOFireWireLibCommandRef
	DeviceCOM::SCreateCompareSwapCommand64(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress* addr, UInt64 cmpVal, 
		UInt64 newVal, IOFireWireLibCommandCallback callback, Boolean failOnReset, UInt32 generation, void* inRefCon,
		REFIID iid )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateCompareSwapCommand(device, 
				addr ? *addr : FWAddress(), cmpVal, newVal, 2, callback, failOnReset, generation, inRefCon, iid) ;
	}

	IOReturn
	DeviceCOM::SGetBusGeneration( IOFireWireLibDeviceRef self, UInt32* outGeneration )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetBusGeneration( outGeneration ) ;
	}
	
	IOReturn
	DeviceCOM::SGetLocalNodeIDWithGeneration( IOFireWireLibDeviceRef self, UInt32 checkGeneration, UInt16* outLocalNodeID )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetLocalNodeIDWithGeneration( checkGeneration, outLocalNodeID ) ;
	}
	
	IOReturn
	DeviceCOM::SGetRemoteNodeID( IOFireWireLibDeviceRef self, UInt32 checkGeneration, UInt16* outRemoteNodeID )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetRemoteNodeID( checkGeneration, outRemoteNodeID ) ;
	}
	
	IOReturn
	DeviceCOM::SGetSpeedToNode( IOFireWireLibDeviceRef self, UInt32 checkGeneration, IOFWSpeed* outSpeed)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetSpeedToNode( checkGeneration, outSpeed ) ;
	}
	
	IOReturn
	DeviceCOM::SGetSpeedBetweenNodes( IOFireWireLibDeviceRef self, UInt32 checkGeneration, UInt16 srcNodeID, UInt16 destNodeID,  IOFWSpeed* outSpeed)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetSpeedBetweenNodes( checkGeneration, srcNodeID, destNodeID, outSpeed ) ;
	}

#pragma mark -
	IOFireWireLibCommandRef	
	DeviceCOM::SCreateReadCommand(IOFireWireLibDeviceRef self,
		io_object_t 		device, 
		const FWAddress*	addr, 
		void* 				buf, 
		UInt32 				size, 
		IOFireWireLibCommandCallback callback,
		Boolean 			failOnReset, 
		UInt32 				generation,
		void*				inRefCon,
		REFIID				iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateReadCommand(device, addr ? *addr : FWAddress(), buf, size, callback, failOnReset, generation, inRefCon, iid) ;
	}
	
	IOFireWireLibCommandRef	
	DeviceCOM::SCreateReadQuadletCommand(IOFireWireLibDeviceRef self, io_object_t device, 
		const FWAddress* addr, UInt32 val[], const UInt32 numQuads, IOFireWireLibCommandCallback callback, 
		Boolean failOnReset, UInt32 generation, void* inRefCon, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateReadQuadletCommand(device, addr ? *addr : FWAddress(), val, numQuads, callback, failOnReset, generation, inRefCon, iid) ;
	}
	
	IOFireWireLibCommandRef
	DeviceCOM::SCreateWriteCommand(IOFireWireLibDeviceRef self, io_object_t device, 
		const FWAddress* addr, void* buf, UInt32 size, IOFireWireLibCommandCallback callback, 
		Boolean failOnReset, UInt32 generation, void* inRefCon, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateWriteCommand( device, addr ? *addr : FWAddress(), buf, size, callback, failOnReset, generation, inRefCon, iid) ;
	}
	
	IOFireWireLibCommandRef
	DeviceCOM::SCreateWriteQuadletCommand(IOFireWireLibDeviceRef self, io_object_t device, 
		const FWAddress* addr, UInt32 quads[], const UInt32 numQuads, IOFireWireLibCommandCallback callback,
		Boolean failOnReset, UInt32 generation, void* inRefCon, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateWriteQuadletCommand(device, addr ? *addr : FWAddress(), quads, numQuads, callback, failOnReset, generation, inRefCon, iid) ;
	}
	
	IOFireWireLibCommandRef
	DeviceCOM::SCreateCompareSwapCommand(IOFireWireLibDeviceRef self, io_object_t device, const FWAddress* addr, UInt32 cmpVal, UInt32 newVal, 
		IOFireWireLibCommandCallback callback, Boolean failOnReset, UInt32 generation, void* inRefCon, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateCompareSwapCommand(device, 
				addr ? *addr : FWAddress(), cmpVal, newVal, 1, callback, failOnReset, generation, inRefCon, iid) ;
	}
	
	IOReturn
	DeviceCOM::SBusReset(IOFireWireLibDeviceRef self)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->BusReset() ;
	}
	

#pragma mark -
	//
	// --- local unit directory support ------------------
	//
	IOFireWireLibLocalUnitDirectoryRef
	DeviceCOM::SCreateLocalUnitDirectory(IOFireWireLibDeviceRef self, REFIID iid)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateLocalUnitDirectory(iid); 
	}
	
	IOFireWireLibConfigDirectoryRef
	DeviceCOM::SGetConfigDirectory(IOFireWireLibDeviceRef self, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetConfigDirectory(iid) ;
	}
	
	IOFireWireLibConfigDirectoryRef
	DeviceCOM::SCreateConfigDirectoryWithIOObject(IOFireWireLibDeviceRef self, io_object_t inObject, REFIID iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateConfigDirectoryWithIOObject(inObject, iid) ;
	}
	
#pragma mark -
	//
	// --- address space support ------------------
	//
	IOFireWireLibPseudoAddressSpaceRef
	DeviceCOM::SCreatePseudoAddressSpace(IOFireWireLibDeviceRef self, UInt32 inLength, void* inRefCon, UInt32 inQueueBufferSize, 
		void* inBackingStore, UInt32 inFlags, REFIID iid)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreatePseudoAddressSpace(inLength, inRefCon, inQueueBufferSize, inBackingStore, inFlags, iid); 
	}
	
	IOFireWireLibPhysicalAddressSpaceRef
	DeviceCOM::SCreatePhysicalAddressSpace(IOFireWireLibDeviceRef self, UInt32 inLength, void* inBackingStore, UInt32 flags, REFIID iid)
	{ 
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreatePhysicalAddressSpace(inLength, inBackingStore, flags, iid); 
	}
	
	IOFireWireLibPseudoAddressSpaceRef
	DeviceCOM::SCreateInitialUnitsPseudoAddressSpace( 
		IOFireWireLibDeviceRef  	self,
		UInt32						inAddressLo,
		UInt32  					inSize, 
		void*  						inRefCon, 
		UInt32  					inQueueBufferSize, 
		void*  						inBackingStore, 
		UInt32  					inFlags,
		REFIID  					iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateInitialUnitsPseudoAddressSpace( inAddressLo, inSize, inRefCon, inQueueBufferSize, inBackingStore, inFlags, iid ) ;
	}
	
	//
	// --- FireLog -----------------------------------
	//
	IOReturn
	DeviceCOM::SFireLog(
		IOFireWireLibDeviceRef self,
		const char *		format, 
		... )
	{
		va_list		ap ;
		va_start( ap, format ) ;
		
		IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->FireLog( format, ap ) ;
		
		va_end( ap ) ;
		
		return kIOReturnSuccess ;
	}
	
	//
	// --- isoch -----------------------------------
	//
	IOReturn
	DeviceCOM::SAddIsochCallbackDispatcherToRunLoop(
		IOFireWireLibDeviceRef	self,
		CFRunLoopRef			runLoop)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->AddIsochCallbackDispatcherToRunLoopForMode( runLoop, kCFRunLoopCommonModes ) ;
	}
	
	IOReturn
	DeviceCOM::SAddIsochCallbackDispatcherToRunLoopForMode( // v3+
		IOFireWireLibDeviceRef 	self, 
		CFRunLoopRef			runLoop,
		CFStringRef 			runLoopMode )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->AddIsochCallbackDispatcherToRunLoopForMode( runLoop, runLoopMode ) ;
	}
	
	void
	DeviceCOM::SRemoveIsochCallbackDispatcherFromRunLoop(	// v3+
		IOFireWireLibDeviceRef 	self)
	{
		IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->RemoveIsochCallbackDispatcherFromRunLoop() ;
	}
	
	IOFireWireLibRemoteIsochPortRef
	DeviceCOM::SCreateRemoteIsochPort(
		IOFireWireLibDeviceRef 	self,
		Boolean					inTalking,
		REFIID					iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateRemoteIsochPort(inTalking, iid) ;
	}
	
	IOFireWireLibLocalIsochPortRef
	DeviceCOM::SCreateLocalIsochPort(
		IOFireWireLibDeviceRef 	self, 
		Boolean					inTalking,
		DCLCommand*		inDCLProgram,
		UInt32					inStartEvent,
		UInt32					inStartState,
		UInt32					inStartMask,
		IOVirtualRange			inDCLProgramRanges[],			// optional optimization parameters
		UInt32					inDCLProgramRangeCount,
		IOVirtualRange			inBufferRanges[],
		UInt32					inBufferRangeCount,
		REFIID 					iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateLocalIsochPortWithOptions( inTalking, inDCLProgram, inStartEvent, inStartState, inStartMask,
												inDCLProgramRanges, inDCLProgramRangeCount, inBufferRanges, 
												inBufferRangeCount, kFWIsochPortDefaultOptions, iid) ;
	}
	
	IOFireWireLibIsochChannelRef
	DeviceCOM::SCreateIsochChannel( IOFireWireLibDeviceRef self, Boolean doIRM, UInt32 packetSize, 
			IOFWSpeed prefSpeed, REFIID iid )
	{
		IOFireWireLibIsochChannelRef result = 0 ;
		
		IUnknownVTbl** iUnknown = reinterpret_cast<IUnknownVTbl**>(IsochChannelCOM::Alloc(*IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self), doIRM, packetSize, prefSpeed)) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	IOFireWireLibDCLCommandPoolRef
	DeviceCOM::SCreateDCLCommandPool( IOFireWireLibDeviceRef self, IOByteCount size, REFIID iid )
	{
		IOFireWireLibDCLCommandPoolRef result = 0 ;
		
		IUnknownVTbl** iUnknown = TraditionalDCLCommandPoolCOM::Alloc(*IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self), size ) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
		
		return result ;
	}
	
	void
	DeviceCOM::SPrintDCLProgram( IOFireWireLibDeviceRef self, const DCLCommand* dcl, UInt32 inDCLCount ) 
	{
		const DCLCommand*		currentDCL	= dcl ;
		UInt32						index		= 0 ;
		
		printf( "IsochPort::printDCLProgram: dcl=%p, inDCLCount=%lud\n", dcl, inDCLCount) ;
		
		while ( (inDCLCount == 0 || index < inDCLCount ) && currentDCL )
		{
			printf( "\n#0x%04lX  @%p   next=%p, cmplrData=0x%08lX, op=%lu ", 
				index, 
				currentDCL,
				currentDCL->pNextDCLCommand,
				currentDCL->compilerData,
				currentDCL->opcode) ;
	
			switch(currentDCL->opcode & ~kFWDCLOpFlagMask)
			{
				case kDCLSendPacketStartOp:
				case kDCLSendPacketWithHeaderStartOp:
				case kDCLSendPacketOp:
				case kDCLReceivePacketStartOp:
				case kDCLReceivePacketOp:
					printf( "(DCLTransferPacket) buffer=%p, size=%lu",
						((DCLTransferPacket*)currentDCL)->buffer,
						((DCLTransferPacket*)currentDCL)->size) ;
					break ;
					
				case kDCLSendBufferOp:
				case kDCLReceiveBufferOp:
					printf( "(DCLTransferBuffer) buffer=%p, size=%lu, packetSize=0x%x, bufferOffset=%08lX",
						((DCLTransferBuffer*)currentDCL)->buffer,
						((DCLTransferBuffer*)currentDCL)->size,
						((DCLTransferBuffer*)currentDCL)->packetSize,
						((DCLTransferBuffer*)currentDCL)->bufferOffset) ;
					break ;
		
				case kDCLCallProcOp:
					printf( "(DCLCallProc) proc=%p, procData=%08lX",
						((DCLCallProc*)currentDCL)->proc,
						((DCLCallProc*)currentDCL)->procData) ;
					break ;
					
				case kDCLLabelOp:
					printf( "(DCLLabel)") ;
					break ;
					
				case kDCLJumpOp:
					printf( "(DCLJump) pJumpDCLLabel=%p",
						((DCLJump*)currentDCL)->pJumpDCLLabel) ;
					break ;
					
				case kDCLSetTagSyncBitsOp:
					printf( "(DCLSetTagSyncBits) tagBits=%04x, syncBits=%04x",
						((DCLSetTagSyncBits*)currentDCL)->tagBits,
						((DCLSetTagSyncBits*)currentDCL)->syncBits) ;
					break ;
					
				case kDCLUpdateDCLListOp:
					printf( "(DCLUpdateDCLList) dclCommandList=%p, numDCLCommands=%lud \n",
						((DCLUpdateDCLList*)currentDCL)->dclCommandList,
						((DCLUpdateDCLList*)currentDCL)->numDCLCommands) ;
					
					for(UInt32 listIndex=0; listIndex < ((DCLUpdateDCLList*)currentDCL)->numDCLCommands; ++listIndex)
					{
						printf( "%p ", (((DCLUpdateDCLList*)currentDCL)->dclCommandList)[listIndex]) ;
					}
					
					break ;
		
				case kDCLPtrTimeStampOp:
					printf( "(DCLPtrTimeStamp) timeStampPtr=%p",
						((DCLPtrTimeStamp*)currentDCL)->timeStampPtr) ;
					break ;
					
				case kDCLSkipCycleOp:
					printf( "(DCLSkipCycle)") ;
					break ;

				case kDCLNuDCLLeaderOp:
					printf( "(DCLNuDCLLeaderOp) DCL pool=%p", ((DCLNuDCLLeader*)currentDCL)->program ) ;
					break ;
			}
			
			currentDCL = currentDCL->pNextDCLCommand ;
			++index ;
		}
		
		printf( "\n") ;
	
		if ( inDCLCount > 0 && index != inDCLCount)
			printf( "unexpected end of program\n") ;
		
		if ( inDCLCount > 0 && currentDCL != NULL)
			printf( "program too long for count\n") ;
	}

	IOReturn
	DeviceCOM::S_GetIRMNodeID( IOFireWireLibDeviceRef self, UInt32 checkGeneration, UInt16* outIRMNodeID )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis( self )->GetIRMNodeID( checkGeneration, outIRMNodeID ) ;
	}

	IOReturn
	DeviceCOM::S_ClipMaxRec2K( IOFireWireLibDeviceRef self, Boolean clipMaxRec )
	{
		//fprintf(stderr, "DeviceCOM::S_ClipMaxRec2K\n") ;
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->ClipMaxRec2K( clipMaxRec) ;		
	}

	IOFireWireLibNuDCLPoolRef
	DeviceCOM::S_CreateNuDCLPool( IOFireWireLibDeviceRef self, UInt32 capacity, REFIID iid )
	{
		IOFireWireLibNuDCLPoolRef	result = 0 ;
		
		const IUnknownVTbl** iUnknown = NuDCLPoolCOM::Alloc( *IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis( self ), capacity ) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
	
		return result ;
	}
	
	IOFireWireLibBufferFillIsochPortRef
	DeviceCOM::S_CreateBufferFillIsochPort( IOFireWireLibDeviceRef self, UInt32 interruptMicroseconds, 
			UInt32 numRanges, IOVirtualRange* ranges, REFIID iid )
	{
		IOFireWireLibBufferFillIsochPortRef	result = 0 ;
		
		IUnknownVTbl** iUnknown = BufferFillIsochPortCOM::Alloc( *IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self),
				interruptMicroseconds, numRanges, ranges ) ;
		if (iUnknown)
		{
			(*iUnknown)->QueryInterface(iUnknown, iid, (void**) & result) ;
			(*iUnknown)->Release(iUnknown) ;
		}
	
		return result ;
	}

	IOFireWireSessionRef
	DeviceCOM::S_GetSessionRef( IOFireWireLibDeviceRef self )
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->GetSessionRef() ;
	}
	
	IOFireWireLibLocalIsochPortRef
	DeviceCOM::S_CreateLocalIsochPortWithOptions(
		IOFireWireLibDeviceRef 	self, 
		Boolean					talking,
		DCLCommand*				dclProgram,
		UInt32					startEvent,
		UInt32					startState,
		UInt32					startMask,
		IOVirtualRange			dclProgramRanges[],			// optional optimization parameters
		UInt32					dclProgramRangeCount,
		IOVirtualRange			bufferRanges[],
		UInt32					bufferRangeCount,
		IOFWIsochPortOptions	options,
		REFIID 					iid)
	{
		return IOFireWireIUnknown::InterfaceMap<DeviceCOM>::GetThis(self)->CreateLocalIsochPortWithOptions( talking, dclProgram, startEvent, startState, startMask,
												dclProgramRanges, dclProgramRangeCount, bufferRanges, 
												bufferRangeCount, options, iid) ;
	}
	
} // namespace