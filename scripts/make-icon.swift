import AppKit
let root=CommandLine.arguments[1]
let fm=FileManager.default
try fm.createDirectory(atPath:root,withIntermediateDirectories:true)
for size in [16,32,128,256,512] {
    for scale in [1,2] {
        let pixels=size*scale
        let image=NSImage(size:NSSize(width:pixels,height:pixels))
        image.lockFocus()
        let factor=CGFloat(pixels)/1024
        let transform=NSAffineTransform();transform.scale(by:factor);transform.concat()
        let bg=NSBezierPath(roundedRect:NSRect(x:40,y:40,width:944,height:944),xRadius:210,yRadius:210)
        NSColor(calibratedRed:0.07,green:0.10,blue:0.12,alpha:1).setFill();bg.fill()
        let colors=[NSColor(calibratedRed:0.45,green:0.91,blue:0.73,alpha:1),NSColor(calibratedRed:0.98,green:0.72,blue:0.37,alpha:1)]
        let heights:[CGFloat]=[190,360,590,440,260]
        for i in 0..<5 {
            let x=CGFloat(228+i*142),h=heights[i]
            let track=NSBezierPath(roundedRect:NSRect(x:x-27,y:512-h/2,width:54,height:h),xRadius:27,yRadius:27)
            colors[i==3 ? 1:0].setFill();track.fill()
        }
        image.unlockFocus()
        let rep=NSBitmapImageRep(data:image.tiffRepresentation!)!
        let name="icon_\(size)x\(size)\(scale==2 ? "@2x":"").png"
        try rep.representation(using:.png,properties:[:])!.write(to:URL(fileURLWithPath:root).appendingPathComponent(name))
    }
}
