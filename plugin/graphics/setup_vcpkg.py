import json
import os
import sys

def patch_vcpkg():
	# Go up 3 levels from plugins/graphics/ to find the engine root, then into vcpkg
	# Assumes structure: nvgt_root/plugin/graphics/setup_vcpkg.py
	# Target: nvgt_root/vcpkg/vcpkg.json
	
	base_dir = os.path.dirname(os.path.abspath(__file__))
	# Adjust this path based on where the plugin sits relative to the engine root
	# Usually: nvgt/plugin/graphics -> go up 2 levels to nvgt/ -> then vcpkg/vcpkg.json
	vcpkg_json_path = os.path.abspath(os.path.join(base_dir, "..", "..", "vcpkg", "vcpkg.json"))

	if not os.path.exists(vcpkg_json_path):
		# Fallback: maybe they put it in extra/plugin/integrated/graphics? (3 levels up)
		vcpkg_json_path_alt = os.path.abspath(os.path.join(base_dir, "..", "..", "..", "vcpkg", "vcpkg.json"))
		if os.path.exists(vcpkg_json_path_alt):
			vcpkg_json_path = vcpkg_json_path_alt
		else:
			print(f"Error: Could not find vcpkg.json at {vcpkg_json_path}")
			print("Please ensure this script is run from within the nvgt directory structure.")
			return

	print(f"Found vcpkg.json at: {vcpkg_json_path}")

	try:
		with open(vcpkg_json_path, 'r') as f:
			data = json.load(f)
		
		dependencies = data.get("dependencies", [])
		modified = False
		
		for dep in dependencies:
			# Check if dependency is sdl3-image (it might be a string or a dict)
			if isinstance(dep, dict) and dep.get("name") == "sdl3-image":
				features = dep.get("features", [])
				
				if "png" not in features:
					features.append("png")
					modified = True
				if "jpeg" not in features:
					features.append("jpeg")
					modified = True
				
				dep["features"] = features
				break
			
			# If it was defined as a simple string "sdl3-image", convert it to object to add features
			elif isinstance(dep, str) and dep == "sdl3-image":
				# Find index to replace
				idx = dependencies.index(dep)
				dependencies[idx] = {
					"name": "sdl3-image",
					"features": ["png", "jpeg"]
				}
				modified = True
				break

		if modified:
			with open(vcpkg_json_path, 'w') as f:
				json.dump(data, f, indent=2)
			print("Successfully updated vcpkg.json to include PNG and JPEG support for sdl3-image.")
		else:
			print("vcpkg.json already has PNG/JPEG support configured.")

	except Exception as e:
		print(f"Failed to patch vcpkg.json: {e}")

if __name__ == "__main__":
	patch_vcpkg()
