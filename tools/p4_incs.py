# P4 build: make every Arduino core library's headers visible to every library
# (the dependency finder misses FS.h / Network.h chains with the pioarduino P4 core).
Import("env")
import glob, os
fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
dirs = sorted(glob.glob(os.path.join(fw, "libraries", "*", "src")))
env.Append(CPPPATH=dirs)
print("p4_incs: %d library include dirs" % len(dirs))
