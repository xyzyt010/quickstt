import base64
up_svg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10'><polygon points='5,2 2,8 8,8' fill='white'/></svg>"
down_svg = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 10 10'><polygon points='2,2 8,2 5,8' fill='white'/></svg>"

print(f"UP: {base64.b64encode(up_svg.encode()).decode()}")
print(f"DOWN: {base64.b64encode(down_svg.encode()).decode()}")
