/* $Id: USBTest.m,v 1.3 2005/09/18 11:25:47 andy Exp $
 *
 * Unless otherwise *explicitly* stated the following text
 * describes the licensed conditions under which the
 * contents of this module release may be distributed:
 *
 * --------------------------------------------------------
 * Redistribution and use in source and binary forms of
 * this module, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain any
 *    existing copyright notice, and this entire permission
 *    notice in its entirety, including the disclaimer of
 *    warranties.
 *
 * 2. Redistributions in binary form must reproduce all
 *    prior and current copyright notices, this list of
 *    conditions, and the following disclaimer in the
 *    documentation and/or other materials provided with
 *    the distribution.
 *
 * 3. The name of any author may not be used to endorse or
 *    promote products derived from this software without
 *    their specific prior written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the
 * terms of the GNU General Public License, in which case
 * the provisions of the GNU GPL are required INSTEAD OF
 * the above restrictions.  (This clause is necessary due
 * to a potential conflict between the GNU GPL and the
 * restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * --------------------------------------------------------
 *
 * Copyright Andy Armstrong, andy@hexten.net, 2005
 */

#import <Foundation/Foundation.h>
#import "K8055.h"

int main (int argc, const char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    K8055 *k8;
    int devs, i;
    
    devs = [K8055 findDevices];
    printf("Available devices: %02x\n", devs);
    
    k8 = [[K8055 alloc] initWithDevice: 0];
    for (i = 0; i < 256; i++) {
        [k8 setOutput: i];
        [k8 setAnalog: i
            onChannel: 0];
        [k8 setAnalog: 255 - i
            onChannel: 1];
    }
    [k8 release];

    //CFRunLoopRun();

    [pool release];
    return 0;
}
