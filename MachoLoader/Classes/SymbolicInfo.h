//
//  SymbolicInfo.h
//  MachoLoader
//
//  Created by Steven Hooley on 24/08/2010.
//  Copyright 2010 Tinsal Parks. All rights reserved.
//



@interface SymbolicInfo : NSObject {

	NSString	*_segmentName;
	NSString	*_sectionName;
	NSUInteger	_address;
	NSString	*_stringValue;
	NSString	*_libraryName;
}

@property (retain) NSString		*segmentName;
@property (retain) NSString		*sectionName;
@property (assign) NSUInteger	address;
@property (retain) NSString		*stringValue;
@property (retain) NSString		*libraryName;
@end
