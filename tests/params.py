import re
import subprocess

class Test:
    def __init__(self):
        self.options = {}
        self.error_count = 0
        self.ignore_params = ["pci_dev", "mangoapp_steam", "fsr_steam_sharpness",
                              "blacklist", "media_player_format"]
        # 这些参数的“默认值”并不在 set_param_defaults() 里（或其在 C++ 中的表示与
        # 配置文本不是 1:1，例如枚举 / keysym / 路径 / 列表 / 带 f 后缀的浮点），
        # 因此只做“是否出现在示例配置中”的存在性检查，不比对取值。
        self.ignore_value_check = [
            "fps_limit", "fps_limit_method", "fps_text", "fps_metrics",
            "media_player_name", "graphs", "legacy_layout", "custom_text",
            "custom_text_center", "exec", "output_folder", "output_file",
            "font_file", "font_file_text", "font_glyph_ranges", "font_size_text",
            "font_scale", "font_scale_media_player", "position",
            "vulkan_present_mode", "gl_size_query", "gl_bind_framebuffer",
            "gl_dont_flip", "toggle_hud", "toggle_hud_position", "toggle_preset",
            "toggle_fps_limit", "toggle_logging", "reset_fps_metrics",
            "reload_cfg", "upload_log", "log_duration", "cpu_text", "gpu_text",
            "autostart_log", "device_battery", "network", "gpu_list", "ftrace",
        ]
        # self.files_changed()
        self.get_options()
        self.get_param_defaults()
        self.find_options_in_readme()
        self.find_options_in_conf()

        if self.error_count > 0:
            print(f"number of errors: {self.error_count}")
            exit(1)

    def get_options(self):
        regex = r"\((.*?)\)"
        with open('../src/overlay_params.h') as f:
            for line in f:
                if ("OVERLAY_PARAM_BOOL" in line or "OVERLAY_PARAM_CUSTOM"
                    in line) and not "#" in line:
                    match = re.search(regex, line)
                    if match:
                        key = match.group(1)
                        if key in self.ignore_params:
                            continue
                        else:
                            self.options[key] = None

    def find_options_in_readme(self):
        with open("../README.md") as f:
            file = f.read()
            for option in self.options:
                if not option in file:
                    self.error_count += 1
                    print(f"Option: {option} is not found in README.md")

    def find_options_in_conf(self):
        with open("../data/MangoHud.conf") as f:
            file = f.read()
            for option, val in self.options.items():

                if not option in file:
                    self.error_count += 1
                    print(f"Option: {option} is not found in MangoHud.conf")

                if option in file:
                    option = "# " + option
                    for line in file.splitlines():
                        if option in line:
                            line = line.strip().split("=")
                            if len(line) != 2:
                                continue

                            key = line[0].strip("#").strip()
                            if key not in self.options:
                                continue

                            if key in self.ignore_value_check:
                                continue
                            if self.options[key] is None:
                                # 默认值未解析出来（不在 set_param_defaults 里）——不比对
                                continue

                            value = line[1].strip()
                            if "," in value:
                                value = value.split(",")

                            if self.options[key] != value:
                                self.error_count += 1
                                print(f"Sample config: option: {key} value is not the same as default")
                                print(f"default: {self.options[key]}, config: {value}")
                                print("")

    @staticmethod
    def _extract_function_body(contents, name):
        """按大括号配平提取 `void <name>(...)` 的函数体；找不到返回 None。"""
        idx = contents.find('void ' + name)
        if idx < 0:
            return None
        br = contents.find('{', idx)
        if br < 0:
            return None
        depth = 0
        for i in range(br, len(contents)):
            c = contents[i]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return contents[br + 1:i]
        return None

    def get_param_defaults(self):
        # Open the C++ file
        with open('../src/overlay_params.cpp', 'r') as f:
            # Read the contents of the file
            contents = f.read()

            # Define the name of the function to search for
            function_name = 'set_param_defaults'

            # 用大括号配平提取函数体。
            # 原实现用非贪婪正则 r"{(.+?)\s*}\s*\n"，会在**第一个**内层 '}' 处截断，
            # 导致绝大多数默认值解析不到（self.options[key] 恒为 None），从而把
            # data/MangoHud.conf 里正常的示例行全部误报成“与默认值不符”。
            function_contents = self._extract_function_body(contents, function_name)

            # If the function is found, extract the contents
            if function_contents is not None:
                # Extract the contents of the function
                pass
                for line in function_contents.splitlines():

                    # FIXME: Some variables get stored as string in a string
                    if not "enabled" in line:
                        line = line.replace("params->", "")
                        line = line.strip().strip(";").split("=")
                        if len(line) != 2:
                            continue

                        key = line[0].strip()
                        value = line[1].strip()
                        if key not in self.options:
                            continue

                        # convert to a list if it contains curly bracket
                        if "{" in value:
                            value = value.replace("{", "").replace("}", "").strip().split(", ")
                            # If option has color in it's name we can assume it's value is
                            # one or more colors and that they are in binary.
                            # We want to convert this from binary because the config
                            # will not be in this format
                            if "color" in key:
                                value = [hex[2:] for hex in value]
                                value = [string.upper() for string in value]

                        # same reasoning as above
                        if "color" in key and type(value) is str:
                            value = value[2:]
                            value = value.upper()

                        if "fps_sampling_period" in key:
                            value = re.sub(r';\s*/\*.*?\*/', '', value)
                            value = str(int(int(value) / 1000000))

                        # if value is a list, make sure we don't store str in str
                        if type(value) == list:
                            value = [element.strip('"') for element in value]

                        self.options[key] = value

Test()
