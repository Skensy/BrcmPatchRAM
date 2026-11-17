#!/usr/bin/ruby
require 'fileutils'
require 'tmpdir'

abort "Usage: firmware_update.tool [Broadcom firmware directory]" if ARGV.length > 1
input = ARGV.first
unless input
  print "Broadcom firmware directory: "
  input = STDIN.gets.to_s.strip
end
input = File.expand_path(input)
abort "Missing bcbtums.inf" unless File.file?(File.join(input, 'bcbtums.inf'))

script = File.expand_path(__FILE__)
Dir.chdir(__dir__)
plists = %w[BrcmPatchRAM BrcmPatchRAM2 BrcmPatchRAM3].map { |name| "BrcmPatchRAM/#{name}-Info.plist" }
targets = ['firmwares'] + plists
pending = '.firmware-update-pending'

# A retry can restore a transaction interrupted between two renames.
restore = lambda do
  targets.reverse_each do |target|
    saved = File.join(pending, 'old', target)
    next unless File.exist?(saved)
    FileUtils.rm_rf(target)
    File.rename(saved, target)
  end
  FileUtils.remove_entry(pending)
end

File.open(script) do |lock|
  lock.flock(File::LOCK_EX)
  restore.call if File.directory?(pending)
  Dir.mktmpdir('.firmware-update-', Dir.pwd) do |stage|
    FileUtils.mkdir_p(File.join(stage, 'old/BrcmPatchRAM'))
    output = File.join(stage, 'firmwares')
    abort "Firmware import failed" unless system('./firmware.rb', input, output)
    plists.each_with_index do |plist, index|
      copy = File.join(stage, plist)
      FileUtils.mkdir_p(File.dirname(copy))
      FileUtils.cp(plist, copy)
      fragment = index.zero? ? 'firmwares.plist' : "firmwares#{index + 1}.plist"
      Dir.chdir(output) do
        abort "Could not clear #{plist}" unless system('/usr/libexec/PlistBuddy', '-c', 'Delete :IOKitPersonalities', copy)
        abort "Could not merge #{plist}" unless system('/usr/libexec/PlistBuddy', '-c', "Merge #{fragment}", copy)
        abort "Invalid #{plist}" unless system('/usr/bin/plutil', '-lint', copy)
      end
    end
    File.rename(stage, pending)
    begin
      targets.each do |target|
        File.rename(target, File.join(pending, 'old', target))
        File.rename(File.join(pending, target), target)
      end
      File.rename(pending, stage)
    ensure
      if File.directory?(pending)
        restore.call
        Dir.mkdir(stage)
      end
    end
  end
end
puts "Firmware and kext plists updated."
