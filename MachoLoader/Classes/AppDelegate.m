//
//  AppDelegate.m
//  MachoLoader
//
//  Created by steve hooley on 03/04/2009.
//  Copyright 2009 BestBefore Ltd. All rights reserved.
//

#import "AppDelegate.h"
#import "MachoLoader.h"
#import "DisassemblyChecker.h"
#import "GenericTimer.h"
#import "FunctionEnumerator.h"
#import "DissasemblyProcessor.h"


@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {

	GenericTimer *readTimer = [[[GenericTimer alloc] init] autorelease];

	NSArray *paths = [NSArray arrayWithObjects:
//					  [[NSBundle mainBundle] executablePath],
//					  @"/Applications/6-386.app/Contents/MacOS/6-386",
//					  @"/Applications/iTunes.app/Contents/MacOS/iTunes_thin",
					  @"/Applications/Adobe Lightroom 3.app/Contents/MacOS/Adobe Lightroomx86_64",
					  @"/Applications/Adobe After Effects CS5/Adobe After Effects CS5.app/Contents/MacOS/After Effects",
					  @"/Library/Frameworks/Houdini.framework/Versions/11.0.469/Houdini",
					  nil];

	for( NSString *each in paths )
	{
		if([[NSFileManager defaultManager] fileExistsAtPath:each])
		{
			
			MachoLoader *ml = [[MachoLoader alloc] initWithPath:each];
			[ml readFile];
			
			DisassemblyChecker *dc = [[DisassemblyChecker alloc] initWithPath:each isFAT:ml->_binaryIsFAT];
			BOOL success = [dc openOTOOL];
			if(!success)
				[NSException raise:@"Failed to open OTOOL" format:@""];

			[ml disassembleWithChecker:dc];
			
			success = [dc close];
			if(!success)
				[NSException raise:@"Failed to close OTOOL" format:@""];
			
			// Wooah! otool reader hijacks standard out?
			NSLog(@"Fin!");
			
//putback later			DissasemblyProcessor *dProcessor = [[DissasemblyProcessor alloc] initWithFunctionEnumerator:[ml functionEnumerator]];
//putback later			[dProcessor processApp];
//putback later			[dProcessor release];
		
			[dc release];
			[ml release];
		}
	}

	[readTimer close];  // 7.1 secs -- 6.7 secs  // 6.2

}

@end
