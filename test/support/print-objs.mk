# Copyright 2026 Courville Software
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Extra makefile (-f) that exposes the object lists of the repo's own Makefile, so test
# tooling builds exactly what the sim target builds instead of keeping a copy of the list.
.PHONY: print-lib-objs print-app-objs
print-lib-objs:
	@echo $(LIB_OBJS)
print-app-objs:
	@echo $(APP_OBJS)
