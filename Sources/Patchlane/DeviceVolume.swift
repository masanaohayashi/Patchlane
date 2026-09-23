import CoreAudio
import AudioToolbox

// Control-queue HAL access only. Never called by the UI or an audio callback.
enum DeviceVolume {
    struct Control {
        var device: AudioObjectID
        var addresses: [AudioObjectPropertyAddress]
        var values: [Float32]
        var scalar: Double { Double(values.max() ?? 0) }
    }
    static func read(_ device: Device) -> Control? {
        func value(_ address: AudioObjectPropertyAddress) -> Float32? {
            var address=address, writable: DarwinBoolean=false
            guard AudioObjectHasProperty(device.id,&address),
                  AudioObjectIsPropertySettable(device.id,&address,&writable)==noErr, writable.boolValue else { return nil }
            var result: Float32=0, size=UInt32(MemoryLayout<Float32>.size)
            guard AudioObjectGetPropertyData(device.id,&address,0,nil,&size,&result)==noErr,
                  result.isFinite, (0...1).contains(result) else { return nil }
            return result
        }
        for selector in [kAudioDevicePropertyVolumeScalar, kAudioHardwareServiceDeviceProperty_VirtualMainVolume] {
            let address=AudioObjectPropertyAddress(mSelector:selector,mScope:kAudioDevicePropertyScopeOutput,mElement:kAudioObjectPropertyElementMain)
            if let scalar=value(address) { return Control(device:device.id,addresses:[address],values:[scalar]) }
        }
        // Some devices expose channel controls instead of a master control.
        guard device.outputs>0 else { return nil }
        let addresses=(1...device.outputs).map { AudioObjectPropertyAddress(mSelector:kAudioDevicePropertyVolumeScalar,mScope:kAudioDevicePropertyScopeOutput,mElement:UInt32($0)) }
        let values=addresses.compactMap(value)
        guard values.count==addresses.count else { return nil }
        return Control(device:device.id,addresses:addresses,values:values)
    }
    static func write(_ control: Control, scalar: Double) -> OSStatus {
        guard scalar.isFinite else { return kAudioHardwareIllegalOperationError }
        let target=Float32(min(1,max(0,scalar))), previous=Float32(control.scalar)
        for index in control.addresses.indices {
            var address=control.addresses[index]
            // Preserve channel balance when providing a synthesized master.
            var value=previous>0 ? target*control.values[index]/previous : target
            let status=AudioObjectSetPropertyData(control.device,&address,0,nil,UInt32(MemoryLayout<Float32>.size),&value)
            if status != noErr { return status }
        }
        return noErr
    }
}
